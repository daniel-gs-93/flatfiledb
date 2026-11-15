#ifndef STRATA_DOMAIN_VALUE_H_
#define STRATA_DOMAIN_VALUE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace strata::domain {

enum class DataType { kInt, kDouble, kText };

// There is no NULL: a column always holds one of these three alternatives.
using Value = std::variant<int64_t, double, std::string>;
using Row = std::vector<Value>;

struct ColumnDef {
  std::string name;
  DataType type;
  // What an INSERT that omits the column stores. Empty when the column has no
  // default, and always empty in a result set.
  std::optional<Value> default_value = std::nullopt;
};

struct ForeignKey {
  std::size_t column_index;
  std::string ref_table;
  std::size_t ref_column_index;
};

struct TableSchema {
  std::string name;
  std::vector<ColumnDef> columns;
  std::size_t primary_key_index;
  std::vector<ForeignKey> foreign_keys;  // ascending by column position
  // Columns declared UNIQUE, not counting the primary key; ascending.
  std::vector<std::size_t> unique_indexes = {};
  // The INT primary key that an INSERT may omit.
  std::optional<std::size_t> auto_increment_index = std::nullopt;
};

enum class ErrorKind {
  kBind,
  kUnsupported,
  kConstraintViolation,
  kRuntime,
  kIo
};

struct ExecutionError {
  ErrorKind kind;
  std::string message;
};

struct AffectedRows {
  std::size_t count;
};

// The outcome of a DDL statement. tag is the text printed after "OK", for
// example "CREATE TABLE users".
struct CommandComplete {
  std::string tag;
  // Lines printed after it (for example the rows an import skipped).
  std::vector<std::string> notes = {};
};

struct ResultSet {
  std::vector<ColumnDef> columns;
  std::vector<Row> rows;
};

using ExecutionResult =
    std::variant<ResultSet, AffectedRows, CommandComplete, ExecutionError>;

// Lower-cases ASCII letters; identifiers are compared case-insensitively.
std::string ToLower(std::string_view text);

DataType TypeOf(const Value& value);

// The value as DOUBLE. Precondition: the value is INT or DOUBLE.
double ToDouble(const Value& value);

// Three-way comparison. Precondition: both values are numeric, or both are
// TEXT. A numeric pair that contains a DOUBLE is compared as DOUBLE.
int CompareValues(const Value& lhs, const Value& rhs);

// Equality under the same rules as CompareValues; unlike operator== on
// std::variant, 2 and 2.0 are equal. Precondition as CompareValues.
bool ValuesEqual(const Value& lhs, const Value& rhs);

// Output form of a value: INT in decimal, DOUBLE as the shortest fixed-notation
// decimal that round-trips and always contains a decimal point, TEXT unchanged.
// Precondition: a DOUBLE value is finite.
std::string FormatValue(const Value& value);

// Converts value to the target type. Only INT to DOUBLE changes the value;
// returns std::nullopt for every other mismatch.
std::optional<Value> CoerceValue(const Value& value, DataType target);

// Lower-case kind label used in CLI output: "bind", "unsupported",
// "constraint violation", "runtime", "io".
const char* ErrorKindName(ErrorKind kind);

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_VALUE_H_
