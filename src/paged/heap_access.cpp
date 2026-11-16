#include "strata/paged/heap_access.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Resource-aware (design document 2.13): this file is where the row layer meets
// the pages. Error mapping is pure; the stream and the reader hold a pointer
// to a TableHeap or a reference to a Database; ApplyChangeSet writes pages.

namespace strata::paged {

namespace {

using domain::ErrorKind;
using domain::ExecutionError;
using domain::Row;

ExecutionError MakeExecutionError(ErrorKind kind, std::string message) {
  return ExecutionError{kind, std::move(message)};
}

ExecutionError RowTooLongError(const std::string& table) {
  return MakeExecutionError(ErrorKind::kUnsupported,
                            "paged engine cannot store a row longer than " +
                                std::to_string(kMaxTupleSize) + " bytes in " +
                                table);
}

ExecutionError InternalError(const PageError& error) {
  return MakeExecutionError(ErrorKind::kIo, "internal error: " + error.message);
}

// ----------------------------------------------------------------------------
// The undo log of ApplyChangeSet (design document 2.13).
// ----------------------------------------------------------------------------

enum class UndoKind { kInserted, kDeleted, kUpdated };

// One action that succeeded. `heap` belongs to the Database; `old_row`
// belongs to the MutationPlan, which outlives the log. kInserted undoes with
// `rid`, the position the insert returned; kDeleted undoes by inserting
// `old_row`; kUpdated undoes with `rid` and `old_row`. A field the kind does
// not use is null or ignored (`rid` of a kDeleted entry is the position the
// row had, which no undo step reads).
struct UndoEntry {
  UndoKind kind;
  TableHeap* heap;
  Rid rid;
  const Row* old_row;
};

using UndoLog = std::vector<UndoEntry>;

ExecutionError RowAccessError(const PageError& error, const TableHeap& heap) {
  return ToExecutionError(
      error, ErrorContext{ErrorSite::kRowAccess, heap.schema().name});
}

// The error of a failed Database::Table. A kNotFound is an unknown table only
// when the catalog has no such table. TableHeap::Open reports a chain that
// leads past the end of the file as kCorrupt, so a kNotFound for a table the
// catalog has is not expected and is an internal error (design document 2.13,
// error mapping). Catalog::FindTable is safe to call from several threads.
ExecutionError OpenTableError(Database& database, const PageError& error,
                              const std::string& table) {
  if (error.code == PageErrorCode::kNotFound &&
      database.catalog().FindTable(table).has_value()) {
    return InternalError(error);
  }
  return ToExecutionError(error, ErrorContext{ErrorSite::kOpenTable, table});
}

// Each Apply function runs one action. Only after the action succeeded does it
// append the matching entry to *log, so the log never names an action that did
// not happen. Returns the mapped error of the failed action, or no value.

std::optional<ExecutionError> ApplyInsert(TableHeap* heap, const Row& row,
                                          UndoLog* log) {
  PageResult<Rid> inserted = heap->Insert(row);
  if (!inserted.ok()) {
    return RowAccessError(inserted.error(), *heap);
  }
  log->push_back(
      UndoEntry{UndoKind::kInserted, heap, inserted.value(), nullptr});
  return std::nullopt;
}

std::optional<ExecutionError> ApplyDelete(TableHeap* heap,
                                          const RowDelete& deletion,
                                          UndoLog* log) {
  if (Status status = heap->Delete(deletion.rid)) {
    return RowAccessError(*status, *heap);
  }
  log->push_back(
      UndoEntry{UndoKind::kDeleted, heap, deletion.rid, &deletion.old_row});
  return std::nullopt;
}

// An update that fits in place keeps its Rid. One that returns kNoSpace is a
// delete followed by an insert, and each half is recorded as it succeeds, so a
// failed insert leaves "Deleted(old_row)" in the log and the old row comes
// back.
std::optional<ExecutionError> ApplyUpdate(TableHeap* heap,
                                          const RowUpdate& update,
                                          UndoLog* log) {
  Status status = heap->Update(update.rid, update.new_row);
  if (!status.has_value()) {
    log->push_back(
        UndoEntry{UndoKind::kUpdated, heap, update.rid, &update.old_row});
    return std::nullopt;
  }
  if (status->code != PageErrorCode::kNoSpace) {
    return RowAccessError(*status, *heap);
  }
  if (Status deleted = heap->Delete(update.rid)) {
    return RowAccessError(*deleted, *heap);
  }
  log->push_back(
      UndoEntry{UndoKind::kDeleted, heap, update.rid, &update.old_row});
  return ApplyInsert(heap, update.new_row, log);
}

std::optional<ExecutionError> ApplyInserts(TableHeap* heap,
                                           const std::vector<Row>& rows,
                                           UndoLog* log) {
  for (const Row& row : rows) {
    if (auto error = ApplyInsert(heap, row, log)) {
      return error;
    }
  }
  return std::nullopt;
}

// One TableChange holds one kind of change; the three loops run in the order
// inserts, updates, deletes.
std::optional<ExecutionError> ApplyTableChange(Database& database,
                                               const TableChange& change,
                                               UndoLog* log) {
  PageResult<TableHeap*> opened = database.Table(change.table);
  if (!opened.ok()) {
    return OpenTableError(database, opened.error(), change.table);
  }
  TableHeap* heap = opened.value();
  if (auto error = ApplyInserts(heap, change.inserts, log)) {
    return error;
  }
  for (const RowUpdate& update : change.updates) {
    if (auto error = ApplyUpdate(heap, update, log)) {
      return error;
    }
  }
  for (const RowDelete& deletion : change.deletes) {
    if (auto error = ApplyDelete(heap, deletion, log)) {
      return error;
    }
  }
  return std::nullopt;
}

// Undoes one entry. An update that no longer fits in its page (an undo insert
// executed earlier may have taken the space) is undone as a delete followed by
// an insert.
Status UndoEntryAction(const UndoEntry& entry) {
  switch (entry.kind) {
    case UndoKind::kInserted:
      return entry.heap->Delete(entry.rid);
    case UndoKind::kDeleted: {
      PageResult<Rid> inserted = entry.heap->Insert(*entry.old_row);
      if (!inserted.ok()) {
        return inserted.error();
      }
      return std::nullopt;
    }
    case UndoKind::kUpdated: {
      Status status = entry.heap->Update(entry.rid, *entry.old_row);
      if (!status.has_value() || status->code != PageErrorCode::kNoSpace) {
        return status;
      }
      if (Status deleted = entry.heap->Delete(entry.rid)) {
        return deleted;
      }
      PageResult<Rid> inserted = entry.heap->Insert(*entry.old_row);
      if (!inserted.ok()) {
        return inserted.error();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// Undoes the log newest first. A failed step does not stop the others. Returns
// the message of the first failed step, or no value when every step worked.
std::optional<std::string> UndoAll(const UndoLog& log) {
  std::optional<std::string> first_failure;
  for (std::size_t i = log.size(); i > 0; --i) {
    Status status = UndoEntryAction(log[i - 1]);
    if (status.has_value() && !first_failure.has_value()) {
      first_failure = status->message;
    }
  }
  return first_failure;
}

// The error of a failed apply, after the log has been undone.
ExecutionError AfterUndo(ExecutionError error, const UndoLog& log) {
  if (std::optional<std::string> failure = UndoAll(log)) {
    error.message += "; rollback failed: " + *failure;
  }
  return error;
}

}  // namespace

ExecutionError ToExecutionError(const PageError& error,
                                const ErrorContext& context) {
  switch (error.code) {
    case PageErrorCode::kIo:
    case PageErrorCode::kNoSpace:
      return MakeExecutionError(ErrorKind::kIo, error.message);
    case PageErrorCode::kCorrupt:
      return MakeExecutionError(ErrorKind::kIo, "corrupt: " + error.message);
    case PageErrorCode::kNoFrame:
      return MakeExecutionError(ErrorKind::kIo,
                                "buffer pool exhausted: " + error.message);
    case PageErrorCode::kClosed:
      return MakeExecutionError(ErrorKind::kIo, "database is closed");
    case PageErrorCode::kNotFound:
      if (context.site == ErrorSite::kOpenTable) {
        return MakeExecutionError(ErrorKind::kBind,
                                  "unknown table: " + context.table);
      }
      return InternalError(error);
    case PageErrorCode::kTooLarge:
      return RowTooLongError(context.table);
    case PageErrorCode::kInvalidArgument:
      if (context.site == ErrorSite::kCreateTable) {
        return MakeExecutionError(ErrorKind::kUnsupported, error.message);
      }
      return InternalError(error);
    case PageErrorCode::kAlreadyExists:
      if (context.site == ErrorSite::kCreateTable) {
        return MakeExecutionError(ErrorKind::kBind,
                                  "table " + context.table + " already exists");
      }
      return InternalError(error);
  }
  return InternalError(error);
}

HeapRowStream::HeapRowStream(const TableHeap* heap)
    : heap_(heap), scan_(heap->Scan()) {}

StreamResult HeapRowStream::Next() {
  PageResult<std::optional<ScanItem>> next = scan_.Next();
  if (!next.ok()) {
    return ToExecutionError(next.error(), ErrorContext{ErrorSite::kRowAccess,
                                                       heap_->schema().name});
  }
  std::optional<ScanItem>& item = next.value();
  if (!item.has_value()) {
    return std::optional<StoredRow>();
  }
  return std::optional<StoredRow>(StoredRow{item->rid, std::move(item->row)});
}

std::vector<domain::ColumnDef> HeapRowStream::Columns() const {
  return heap_->schema().columns;
}

ScanResult DatabaseTableReader::Scan(const std::string& table) const {
  PageResult<TableHeap*> opened = database_.Table(table);
  if (!opened.ok()) {
    return OpenTableError(database_, opened.error(), table);
  }
  return std::unique_ptr<IRowStream>(
      std::make_unique<HeapRowStream>(opened.value()));
}

std::optional<ExecutionError> ApplyChangeSet(Database& database,
                                             const MutationPlan& plan) {
  UndoLog log;
  for (const TableChange& change : plan.changes) {
    if (auto error = ApplyTableChange(database, change, &log)) {
      return AfterUndo(std::move(*error), log);
    }
  }
  return std::nullopt;
}

std::optional<ExecutionError> InsertRows(Database& database,
                                         const std::string& table,
                                         const std::vector<Row>& rows) {
  PageResult<TableHeap*> opened = database.Table(table);
  if (!opened.ok()) {
    return OpenTableError(database, opened.error(), table);
  }
  UndoLog log;
  if (auto error = ApplyInserts(opened.value(), rows, &log)) {
    return AfterUndo(std::move(*error), log);
  }
  return std::nullopt;
}

}  // namespace strata::paged
