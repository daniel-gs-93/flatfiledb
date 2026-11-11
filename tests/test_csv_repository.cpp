// Integration tests of CsvRepository (docs/TestDocument.md section 3.2) against
// database directories in a temporary location.

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/csv_codec.h"
#include "test_harness.h"
#include "test_support.h"

namespace flatfiledb::testing {

namespace {

using infrastructure::CommitSet;
using infrastructure::CsvRepository;
using infrastructure::LoadedTable;
using infrastructure::TableWrite;

constexpr const char* kUsersHeader = "id,username,location";
constexpr const char* kSuccess = "<success>";

domain::Value Int(std::int64_t value) { return value; }
domain::Value Text(const std::string& value) { return value; }

domain::ColumnDef Column(const std::string& name, domain::DataType type) {
  return domain::ColumnDef{name, type};
}

domain::TableSchema NotesSchema() {
  return domain::TableSchema{"notes",
                             {Column("id", domain::DataType::kInt),
                              Column("body", domain::DataType::kText)},
                             0,
                             {}};
}

// Renders a failure description, so a CHECK_EQ against kSuccess shows it.
std::string DescribeOutcome(const std::optional<std::string>& failure) {
  return failure.value_or(kSuccess);
}

// The tables of a successful load, or an empty list after reporting why the
// load failed.
std::vector<LoadedTable> MustLoad(CsvRepository& repository,
                                  bool is_parallel = false) {
  infrastructure::LoadOutcome outcome = repository.Load(is_parallel);
  if (const auto* error = std::get_if<std::string>(&outcome)) {
    ReportFailure(__FILE__, __LINE__, "load failed: " + *error);
    return {};
  }
  return std::move(std::get<std::vector<LoadedTable>>(outcome));
}

// The reason a load failed, or kSuccess after reporting that it succeeded.
std::string LoadFailure(CsvRepository& repository, bool is_parallel) {
  infrastructure::LoadOutcome outcome = repository.Load(is_parallel);
  if (const auto* error = std::get_if<std::string>(&outcome)) {
    return *error;
  }
  ReportFailure(__FILE__, __LINE__, "the load succeeded");
  return kSuccess;
}

// The rows as CSV lines, in order.
std::string DescribeRows(const std::vector<domain::Row>& rows) {
  std::string text;
  for (const domain::Row& row : rows) {
    text += infrastructure::SerializeRow(row) + "\n";
  }
  return text;
}

// The rows of the named table, or a marker when the load has no such table.
std::string DescribeTable(const std::vector<LoadedTable>& tables,
                          const std::string& name) {
  for (const LoadedTable& table : tables) {
    if (table.schema.name == name) {
      return DescribeRows(table.rows);
    }
  }
  return "<no table " + name + ">";
}

std::optional<std::string> CommitWrites(CsvRepository& repository,
                                        std::vector<TableWrite> writes) {
  CommitSet set;
  set.writes = std::move(writes);
  return repository.Commit(set);
}

// The sorted names of the entries in dir, joined by ",".
std::string ListDirectory(const std::filesystem::path& dir) {
  std::vector<std::string> names;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    names.push_back(entry.path().filename().string());
  }
  std::ranges::sort(names);
  std::string joined;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      joined += ',';
    }
    joined += names[i];
  }
  return joined;
}

constexpr const char* kFixtureAListing =
    "catalog.csv,engagements.csv,posts.csv,users.csv";

std::size_t CountLines(const std::string& text) {
  std::size_t line_count =
      static_cast<std::size_t>(std::ranges::count(text, '\n'));
  if (!text.empty() && text.back() != '\n') {
    ++line_count;
  }
  return line_count;
}

domain::Row UserRow(int id, const std::string& username,
                    const std::string& location) {
  return domain::Row{Int(id), Text(username), Text(location)};
}

domain::Row PostRow(int id, const std::string& content,
                    const std::string& username, int views) {
  return domain::Row{Int(id), Text(content), Text(username), Int(views)};
}

domain::Row EngagementRow(int id, int post_id, const std::string& username,
                          const std::string& type, const std::string& comment,
                          int timestamp) {
  return domain::Row{Int(id),    Int(post_id),  Text(username),
                     Text(type), Text(comment), Int(timestamp)};
}

// The rows of Fixture A, in file order.
std::vector<domain::Row> FixtureAUsersRows() {
  return {UserRow(1, "alice", "Paris"), UserRow(2, "carol", "Paris"),
          UserRow(3, "bob", "Rome"), UserRow(4, "dave", "Oslo")};
}

std::vector<domain::Row> FixtureAEngagementsRows() {
  return {EngagementRow(100, 10, "bob", "like", "None", 1000),
          EngagementRow(101, 10, "carol", "comment", "nice", 1001),
          EngagementRow(102, 11, "bob", "comment", "ok", 1002),
          EngagementRow(103, 12, "alice", "like", "None", 1003),
          EngagementRow(104, 12, "carol", "like", "None", 1004)};
}

// Every table of a load written back, as the writes of one commit.
std::vector<TableWrite> AllTableWrites(const std::vector<LoadedTable>& tables) {
  std::vector<TableWrite> writes;
  for (const LoadedTable& table : tables) {
    writes.push_back(TableWrite{table.schema, table.rows});
  }
  return writes;
}

// Serial and parallel loads of the same directory give the same tables.
void ExpectSerialEqualsParallel(const std::filesystem::path& dir) {
  constexpr int kParallelRuns =
      20;  // Repeated to give a data race a chance to show.
  auto repository = MakeCsvRepository(dir);
  std::vector<LoadedTable> serial = MustLoad(*repository, false);
  CHECK_EQ(serial.size(), std::size_t{3});

  for (int run = 0; run < kParallelRuns; ++run) {
    std::vector<LoadedTable> parallel = MustLoad(*repository, true);
    CHECK_EQ(parallel.size(), serial.size());
    for (const char* name : {"users", "posts", "engagements"}) {
      CHECK_EQ(DescribeTable(parallel, name), DescribeTable(serial, name));
    }
  }
}

// Fixture A without the file named missing_file fails both loads.
void ExpectLoadFailsWithout(const std::string& missing_file) {
  TempDir dir;
  WriteFixtureA(dir.path());
  std::error_code error;
  std::filesystem::remove(dir.path() / missing_file, error);
  CHECK(!error);
  auto repository = MakeCsvRepository(dir.path());

  CHECK_EQ(LoadFailure(*repository, false), "cannot read " + missing_file);
  CHECK_EQ(LoadFailure(*repository, true), "cannot read " + missing_file);
}

// Appends row to a users.csv holding initial_text.
void ExpectAppendOutcome(const std::string& initial_text,
                         const domain::Row& row,
                         const std::string& expected_text) {
  TempDir dir;
  WriteFile(dir.path() / "users.csv", initial_text);
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> failure =
      repository->AppendRows(UsersSchema(), {row});

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  std::string actual_text = ReadFile(dir.path() / "users.csv");
  CHECK_EQ(actual_text, expected_text);
  CHECK_EQ(CountLines(actual_text), CountLines(initial_text) + 1);
}

// Writes one users row over the users.csv state the caller prepared in
// dir (empty or absent) and expects a header generated from the schema.
void ExpectGeneratedUsersHeader(const std::filesystem::path& dir) {
  auto repository = MakeCsvRepository(dir);

  std::optional<std::string> failure = CommitWrites(
      *repository, {TableWrite{UsersSchema(), {UserRow(1, "alice", "Paris")}}});

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  std::string text = ReadFile(dir / "users.csv");
  CHECK_EQ(text.substr(0, text.find('\n')), kUsersHeader);
  CHECK_EQ(text, "id,username,location\n1,alice,Paris\n");
  CHECK(text.find('\r') == std::string::npos);
}

void SetPermissions(const std::filesystem::path& path,
                    std::filesystem::perms permissions) {
  std::error_code error;
  std::filesystem::permissions(path, permissions,
                               std::filesystem::perm_options::replace, error);
  CHECK(!error);
}

unsigned PermissionBits(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::file_status status = std::filesystem::status(path, error);
  CHECK(!error);
  return static_cast<unsigned>(status.permissions() &
                               std::filesystem::perms::mask);
}

// Rewrites the three Fixture A table files in dir with CRLF line endings.
void ConvertFixtureAToCrlf(const std::filesystem::path& dir) {
  for (const char* file_name : {"users.csv", "posts.csv", "engagements.csv"}) {
    std::string crlf_text;
    for (char character : ReadFile(dir / file_name)) {
      if (character == '\n') {
        crlf_text += '\r';
      }
      crlf_text += character;
    }
    WriteFile(dir / file_name, crlf_text);
  }
}

// A CRLF copy of Fixture A loads to the same tables as the LF original.
void ExpectCrlfLoadMatchesLf(bool is_parallel) {
  TempDir lf_dir;
  WriteFixtureA(lf_dir.path());
  TempDir crlf_dir;
  WriteFixtureA(crlf_dir.path());
  ConvertFixtureAToCrlf(crlf_dir.path());
  CHECK(ReadFile(crlf_dir.path() / "users.csv").find('\r') !=
        std::string::npos);
  auto lf_repository = MakeCsvRepository(lf_dir.path());
  auto crlf_repository = MakeCsvRepository(crlf_dir.path());

  std::vector<LoadedTable> lf_tables = MustLoad(*lf_repository, is_parallel);
  std::vector<LoadedTable> crlf_tables =
      MustLoad(*crlf_repository, is_parallel);

  CHECK_EQ(lf_tables.size(), std::size_t{3});
  for (const char* name : {"users", "posts", "engagements"}) {
    CHECK_EQ(DescribeTable(crlf_tables, name), DescribeTable(lf_tables, name));
  }
}

// Fixture A where the file named replaced_file is a directory fails both
// loads.
void ExpectLoadFailsWithDirectoryAt(const std::string& replaced_file) {
  TempDir dir;
  WriteFixtureA(dir.path());
  std::error_code error;
  std::filesystem::remove(dir.path() / replaced_file, error);
  std::filesystem::create_directory(dir.path() / replaced_file, error);
  CHECK(!error);
  auto repository = MakeCsvRepository(dir.path());

  CHECK_EQ(LoadFailure(*repository, false), "cannot read " + replaced_file);
  CHECK_EQ(LoadFailure(*repository, true), "cannot read " + replaced_file);
}

// AppendRows over a users.csv the caller prepared in dir (empty or absent)
// writes the schema header before the row.
void ExpectAppendWritesHeader(const std::filesystem::path& dir) {
  auto repository = MakeCsvRepository(dir);

  std::optional<std::string> failure =
      repository->AppendRows(UsersSchema(), {UserRow(5, "eve", "Rome")});

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  CHECK_EQ(ReadFile(dir / "users.csv"), "id,username,location\n5,eve,Rome\n");
}

// Fixture C1 with the users.csv replaced by users_text.
void WriteFixtureWithUsers(const std::filesystem::path& dir,
                           const std::string& users_text) {
  WriteFixtureC1(dir);
  WriteFile(dir / "users.csv", users_text);
}

// A directory whose non-empty content makes std::filesystem::remove fail, so
// the cleanup after a failed commit leaves it in place.
void MakeUnremovableDirectory(const std::filesystem::path& path) {
  WriteFile(path / "keep", "x");
}

}  // namespace

TEST(test_csv_load_rejects_invalid_rows) {
  TempDir dir;
  WriteFixtureB(dir.path());
  auto repository = MakeCsvRepository(dir.path());

  std::vector<LoadedTable> tables = MustLoad(*repository);

  CHECK_EQ(tables.size(), std::size_t{3});
  // Duplicate id 1 keeps the last row; the rows "x,..." and "5,eve" are
  // rejected.
  CHECK_EQ(DescribeTable(tables, "users"), "1,alicia,Rome\n2,bob,Rome\n");
  // The row 11 with views "abc" is rejected. Post 10 names "alice", who is not
  // a user: loading does not check foreign keys.
  CHECK_EQ(DescribeTable(tables, "posts"), "10,hello,alice,5\n");
  // Engagements 101 (no post 99) and 102 (no user "zed") are loaded as well.
  CHECK_EQ(DescribeTable(tables, "engagements"),
           "100,10,bob,like,None,1\n"
           "101,99,bob,like,None,2\n"
           "102,10,zed,like,None,3\n"
           "103,10,alicia,comment,hi,4\n");
}

TEST(test_csv_load_parallel_equals_serial) {
  TempDir fixture_a_dir;
  WriteFixtureA(fixture_a_dir.path());
  TempDir fixture_b_dir;
  WriteFixtureB(fixture_b_dir.path());

  ExpectSerialEqualsParallel(fixture_a_dir.path());
  ExpectSerialEqualsParallel(fixture_b_dir.path());
}

TEST(test_csv_load_missing_table_file_returns_description) {
  ExpectLoadFailsWithout("users.csv");
  ExpectLoadFailsWithout("posts.csv");
  ExpectLoadFailsWithout("engagements.csv");
}

TEST(test_csv_commit_keeps_header_and_replaces_rows) {
  TempDir dir;
  WriteFixtureA(dir.path());
  auto repository = MakeCsvRepository(dir.path());
  std::vector<domain::Row> users_rows = FixtureAUsersRows();
  users_rows[0][1] = Text("zed");
  std::vector<domain::Row> engagements_rows = FixtureAEngagementsRows();
  engagements_rows[0][3] = Text("x");

  std::optional<std::string> failure = CommitWrites(
      *repository, {TableWrite{UsersSchema(), users_rows},
                    TableWrite{EngagementsSchema(), engagements_rows}});

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  // Only the changed cells differ from Fixture A. The header keeps the file's
  // spelling "postid"; the schema spells it "postId".
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           "id,username,location\n"
           "1,zed,Paris\n"
           "2,carol,Paris\n"
           "3,bob,Rome\n"
           "4,dave,Oslo\n");
  CHECK_EQ(ReadFile(dir.path() / "engagements.csv"),
           "id,postid,username,type,comment,timestamp\n"
           "100,10,bob,x,None,1000\n"
           "101,10,carol,comment,nice,1001\n"
           "102,11,bob,comment,ok,1002\n"
           "103,12,alice,like,None,1003\n"
           "104,12,carol,like,None,1004\n");
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_commit_preserves_line_endings) {
  TempDir dir;
  WriteFile(dir.path() / "users.csv",
            "id,username,location\n1,alice,Paris\n2,bob,Rome\n");
  WriteFile(dir.path() / "posts.csv",
            "id,content,username,views\r\n10,hello world,alice,5\r\n11,second "
            "post,alice,0\r\n");
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> lf_failure = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(),
                  {UserRow(1, "alice", "Lyon"), UserRow(2, "bob", "Rome")}}});
  std::optional<std::string> crlf_failure = CommitWrites(
      *repository, {TableWrite{PostsSchema(),
                               {PostRow(10, "hello world", "alice", 6),
                                PostRow(11, "second post", "alice", 0)}}});

  CHECK_EQ(DescribeOutcome(lf_failure), kSuccess);
  CHECK_EQ(DescribeOutcome(crlf_failure), kSuccess);
  // Exact text: the header and every row use the file's own ending, no mixing.
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           "id,username,location\n1,alice,Lyon\n2,bob,Rome\n");
  CHECK_EQ(ReadFile(dir.path() / "posts.csv"),
           "id,content,username,views\r\n10,hello world,alice,6\r\n11,second "
           "post,alice,0\r\n");
}

TEST(test_csv_append_rows_adds_newline_when_missing) {
  const domain::Row new_row = UserRow(5, "eve", "Rome");

  ExpectAppendOutcome("id,username,location\n1,alice,Paris\n", new_row,
                      "id,username,location\n1,alice,Paris\n5,eve,Rome\n");
  ExpectAppendOutcome("id,username,location\n1,alice,Paris", new_row,
                      "id,username,location\n1,alice,Paris\n5,eve,Rome\n");
  ExpectAppendOutcome(
      "id,username,location\r\n1,alice,Paris\r\n", new_row,
      "id,username,location\r\n1,alice,Paris\r\n5,eve,Rome\r\n");
}

TEST(test_csv_commit_staging_failure_leaves_all_files_unchanged) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  const std::string engagements_before =
      ReadFile(dir.path() / "engagements.csv");
  // A directory where engagements.csv.temp must be written: the users table is
  // staged first and succeeds, the engagements table then fails.
  MakeUnremovableDirectory(dir.path() / "engagements.csv.temp");
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> failure = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(),
                  {UserRow(1, "zed", "Paris"), UserRow(2, "bob", "Rome")}},
       TableWrite{EngagementsSchema(),
                  {EngagementRow(100, 10, "bob", "x", "None", 1000)}}});

  CHECK(failure.has_value());
  CHECK(failure.value_or("").find("engagements.csv") != std::string::npos);
  CHECK_EQ(ReadFile(dir.path() / "users.csv"), users_before);
  CHECK_EQ(ReadFile(dir.path() / "engagements.csv"), engagements_before);
  // users.csv.temp was written before engagements failed and must be gone.
  CHECK_EQ(ListDirectory(dir.path()),
           "catalog.csv,engagements.csv,engagements.csv.temp,posts.csv,"
           "users.csv");
}

TEST(test_csv_commit_generates_header_when_file_empty_or_missing) {
  TempDir empty_file_dir;
  WriteFile(empty_file_dir.path() / "users.csv", "");
  TempDir missing_file_dir;

  ExpectGeneratedUsersHeader(empty_file_dir.path());
  ExpectGeneratedUsersHeader(missing_file_dir.path());
}

TEST(test_csv_commit_drops_rows_rejected_at_load) {
  TempDir dir;
  WriteFixtureB(dir.path());
  auto repository = MakeCsvRepository(dir.path());
  std::vector<LoadedTable> first_load = MustLoad(*repository);

  std::optional<std::string> failure =
      CommitWrites(*repository, AllTableWrites(first_load));
  std::vector<LoadedTable> second_load = MustLoad(*repository);

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  CHECK_EQ(second_load.size(), first_load.size());
  for (const char* name : {"users", "posts", "engagements"}) {
    CHECK_EQ(DescribeTable(second_load, name), DescribeTable(first_load, name));
  }
  // Header plus the loaded rows only: the rejected rows are not written back.
  std::string users_text = ReadFile(dir.path() / "users.csv");
  CHECK_EQ(users_text, "id,username,location\n1,alicia,Rome\n2,bob,Rome\n");
  CHECK_EQ(CountLines(users_text), std::size_t{3});
  CHECK_EQ(CountLines(ReadFile(dir.path() / "posts.csv")), std::size_t{2});
  CHECK_EQ(CountLines(ReadFile(dir.path() / "engagements.csv")),
           std::size_t{5});
}

TEST(test_csv_commit_refuses_leftover_orig) {
  TempDir dir;
  WriteFixtureA(dir.path());
  WriteFile(dir.path() / "engagements.csv.orig", "stale\n");
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  const std::string engagements_before =
      ReadFile(dir.path() / "engagements.csv");
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> failure = CommitWrites(
      *repository, {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}},
                    TableWrite{EngagementsSchema(), {}}});

  CHECK_EQ(DescribeOutcome(failure), "leftover engagements.csv.orig");
  CHECK_EQ(ReadFile(dir.path() / "users.csv"), users_before);
  CHECK_EQ(ReadFile(dir.path() / "engagements.csv"), engagements_before);
  CHECK_EQ(ReadFile(dir.path() / "engagements.csv.orig"), "stale\n");
  // The users table, checked before the leftover, has no .temp either.
  CHECK_EQ(ListDirectory(dir.path()),
           "catalog.csv,engagements.csv,engagements.csv.orig,posts.csv,"
           "users.csv");
}

TEST(test_csv_load_strips_carriage_returns) {
  ExpectCrlfLoadMatchesLf(false);
  ExpectCrlfLoadMatchesLf(true);
}

TEST(test_csv_append_rows_on_empty_file_writes_header) {
  TempDir empty_file_dir;
  WriteFile(empty_file_dir.path() / "users.csv", "");
  TempDir missing_file_dir;

  ExpectAppendWritesHeader(empty_file_dir.path());
  ExpectAppendWritesHeader(missing_file_dir.path());
}

TEST(test_csv_commit_reserved_table_name_returns_description) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string catalog_before = ReadFile(dir.path() / "catalog.csv");
  auto repository = MakeCsvRepository(dir.path());
  domain::TableSchema reserved = NotesSchema();
  reserved.name = "Catalog";

  std::optional<std::string> failure = CommitWrites(
      *repository, {TableWrite{reserved, {domain::Row{Int(1), Text("x")}}}});

  CHECK_EQ(DescribeOutcome(failure), "reserved table name catalog");
  CHECK_EQ(ReadFile(dir.path() / "catalog.csv"), catalog_before);
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_commit_duplicate_table_returns_description) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  auto repository = MakeCsvRepository(dir.path());
  CommitSet dropped_and_written;
  dropped_and_written.writes = {
      TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}}};
  dropped_and_written.drops = {"users"};

  std::optional<std::string> twice = CommitWrites(
      *repository, {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}},
                    TableWrite{UsersSchema(), {UserRow(2, "yan", "Rome")}}});
  std::optional<std::string> written_and_dropped =
      repository->Commit(dropped_and_written);

  CHECK_EQ(DescribeOutcome(twice), "duplicate table users");
  CHECK_EQ(DescribeOutcome(written_and_dropped), "duplicate table users");
  CHECK_EQ(ReadFile(dir.path() / "users.csv"), users_before);
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_append_rows_reserved_table_name_returns_description) {
  TempDir dir;
  WriteFixtureA(dir.path());
  auto repository = MakeCsvRepository(dir.path());
  domain::TableSchema reserved = NotesSchema();
  reserved.name = "catalog";

  std::optional<std::string> failure =
      repository->AppendRows(reserved, {{Int(1), Text("x")}});

  CHECK_EQ(DescribeOutcome(failure), "reserved table name catalog");
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_load_directory_instead_of_file_returns_description) {
  ExpectLoadFailsWithDirectoryAt("users.csv");
  ExpectLoadFailsWithDirectoryAt("posts.csv");
  ExpectLoadFailsWithDirectoryAt("engagements.csv");
}

TEST(test_csv_commit_refuses_directory_at_csv_path) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  std::error_code error;
  std::filesystem::remove(dir.path() / "posts.csv", error);
  std::filesystem::create_directory(dir.path() / "posts.csv", error);
  CHECK(!error);
  WriteFile(dir.path() / "posts.csv" / "inner.txt", "keep\n");
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> write_failure = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}},
       TableWrite{PostsSchema(), {PostRow(10, "hello", "alice", 5)}}});
  std::optional<std::string> append_failure =
      repository->AppendRows(PostsSchema(), {PostRow(10, "hello", "alice", 5)});

  CHECK_EQ(DescribeOutcome(write_failure), "not a regular file: posts.csv");
  CHECK_EQ(DescribeOutcome(append_failure), "not a regular file: posts.csv");
  CHECK(std::filesystem::is_directory(dir.path() / "posts.csv"));
  CHECK_EQ(ListDirectory(dir.path() / "posts.csv"), "inner.txt");
  CHECK_EQ(ReadFile(dir.path() / "posts.csv" / "inner.txt"), "keep\n");
  CHECK_EQ(ReadFile(dir.path() / "users.csv"), users_before);
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_commit_refuses_symlink) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  std::error_code error;
  std::filesystem::rename(dir.path() / "users.csv",
                          dir.path() / "real_users.csv", error);
  CHECK(!error);
  std::filesystem::create_symlink("real_users.csv", dir.path() / "users.csv",
                                  error);
  CHECK(!error);
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> write_failure = CommitWrites(
      *repository, {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}}});
  std::optional<std::string> append_failure =
      repository->AppendRows(UsersSchema(), {UserRow(5, "eve", "Rome")});

  CHECK_EQ(DescribeOutcome(write_failure), "not a regular file: users.csv");
  CHECK_EQ(DescribeOutcome(append_failure), "not a regular file: users.csv");
  CHECK(std::filesystem::is_symlink(dir.path() / "users.csv"));
  CHECK_EQ(ReadFile(dir.path() / "real_users.csv"), users_before);
  CHECK_EQ(ListDirectory(dir.path()),
           "catalog.csv,engagements.csv,posts.csv,real_users.csv,users.csv");
}

TEST(test_csv_commit_preserves_file_permissions) {
  using std::filesystem::perms;
  TempDir dir;
  WriteFixtureA(dir.path());
  const perms group_readable =
      perms::owner_read | perms::owner_write | perms::group_read;
  const perms owner_only = perms::owner_read | perms::owner_write;
  SetPermissions(dir.path() / "posts.csv", group_readable);
  SetPermissions(dir.path() / "users.csv", owner_only);
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> failure = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}},
       TableWrite{PostsSchema(), {PostRow(10, "hello", "alice", 6)}}});

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  CHECK_EQ(ReadFile(dir.path() / "posts.csv"),
           "id,content,username,views\n10,hello,alice,6\n");
  CHECK_EQ(PermissionBits(dir.path() / "posts.csv"), 0640u);
  CHECK_EQ(PermissionBits(dir.path() / "users.csv"), 0600u);
  // Restore write access before TempDir removes the tree.
  SetPermissions(dir.path() / "posts.csv",
                 perms::owner_read | perms::owner_write);
}

TEST(test_csv_commit_refuses_read_only_file) {
  // Arrange
  using std::filesystem::perms;
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string users_before = ReadFile(dir.path() / "users.csv");
  const std::string posts_before = ReadFile(dir.path() / "posts.csv");
  SetPermissions(dir.path() / "posts.csv",
                 perms::owner_read | perms::group_read | perms::others_read);
  auto repository = MakeCsvRepository(dir.path());

  // Act
  std::optional<std::string> failure = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "zed", "Paris")}},
       TableWrite{PostsSchema(), {PostRow(10, "hello", "alice", 6)}}});

  // Assert
  CHECK_EQ(DescribeOutcome(failure), "could not write posts.csv");
  CHECK_EQ(ReadFile(dir.path() / "users.csv"), users_before);
  CHECK_EQ(ReadFile(dir.path() / "posts.csv"), posts_before);
  CHECK_EQ(PermissionBits(dir.path() / "posts.csv"), 0444u);
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
}

TEST(test_csv_load_without_catalog_returns_empty_database) {
  TempDir dir;
  WriteFile(dir.path() / "users.csv", "id,username,location\n1,alice,Paris\n");
  auto repository = MakeCsvRepository(dir.path());

  std::vector<LoadedTable> serial = MustLoad(*repository, false);
  std::vector<LoadedTable> parallel = MustLoad(*repository, true);

  CHECK(serial.empty());
  CHECK(parallel.empty());
}

TEST(test_csv_load_invalid_catalog_returns_description) {
  TempDir two_keys_dir;
  WriteFile(two_keys_dir.path() / "catalog.csv",
            "table,position,column,type,primary_key,unique,auto_increment,has_"
            "default,default,ref_table,ref_column\n"
            "users,0,id,INT,1,0,0,0,,,\n"
            "users,1,name,TEXT,1,0,0,0,,,\n");
  TempDir directory_dir;
  std::filesystem::create_directory(directory_dir.path() / "catalog.csv");
  auto two_keys = MakeCsvRepository(two_keys_dir.path());
  auto directory = MakeCsvRepository(directory_dir.path());

  const std::string invalid = LoadFailure(*two_keys, false);
  const std::string unreadable = LoadFailure(*directory, false);

  CHECK_EQ(invalid.substr(0, std::string("invalid catalog.csv").size()),
           "invalid catalog.csv");
  CHECK_EQ(unreadable, "cannot read catalog.csv");
}

TEST(test_csv_load_header_mismatch_returns_description) {
  TempDir short_header_dir;
  WriteFixtureA(short_header_dir.path());
  WriteFile(short_header_dir.path() / "posts.csv",
            "id,content,username\n10,hello,alice\n");
  TempDir case_dir;
  WriteFixtureA(case_dir.path());
  WriteFile(case_dir.path() / "users.csv",
            "ID,UserName,LOCATION\n1,alice,Paris\n");
  auto short_header = MakeCsvRepository(short_header_dir.path());
  auto different_case = MakeCsvRepository(case_dir.path());

  const std::string failure = LoadFailure(*short_header, false);
  std::vector<LoadedTable> tables = MustLoad(*different_case);

  CHECK_EQ(failure, "header of posts.csv does not match the catalog");
  CHECK_EQ(DescribeTable(tables, "users"), "1,alice,Paris\n");
}

TEST(test_csv_load_ignores_files_not_in_catalog) {
  TempDir plain_dir;
  WriteFixtureA(plain_dir.path());
  TempDir extra_dir;
  WriteFixtureA(extra_dir.path());
  WriteFile(extra_dir.path() / "extra.csv", "a,b\n1,2\n");
  WriteFile(extra_dir.path() / "users.csv.orig", "stale\n");
  WriteFile(extra_dir.path() / ".flatfiledb.lock", "");
  WriteFile(extra_dir.path() / "notes.txt", "hello\n");
  auto plain = MakeCsvRepository(plain_dir.path());
  auto extra = MakeCsvRepository(extra_dir.path());

  std::vector<LoadedTable> expected = MustLoad(*plain);
  std::vector<LoadedTable> actual = MustLoad(*extra);

  CHECK_EQ(actual.size(), expected.size());
  for (const char* name : {"users", "posts", "engagements"}) {
    CHECK_EQ(DescribeTable(actual, name), DescribeTable(expected, name));
  }
}

TEST(test_csv_load_keeps_file_order_and_last_duplicate) {
  TempDir dir;
  WriteFixtureWithUsers(dir.path(),
                        "id,username,location\n3,c,x\n1,a,x\n3,d,y\n2,b,x\n");
  auto repository = MakeCsvRepository(dir.path());

  std::vector<LoadedTable> tables = MustLoad(*repository);

  CHECK_EQ(DescribeTable(tables, "users"), "1,a,x\n3,d,y\n2,b,x\n");
}

TEST(test_csv_load_reads_quoted_fields) {
  TempDir dir;
  WriteFixtureD(dir.path());
  auto repository = MakeCsvRepository(dir.path());

  std::vector<LoadedTable> tables = MustLoad(*repository);

  CHECK_EQ(tables.size(), std::size_t{1});
  if (tables.size() != 1) {
    return;
  }
  // The lines "5,\"unterminated" and "6,\"ok\"x" are rejected.
  CHECK_EQ(tables[0].rows.size(), std::size_t{4});
  if (tables[0].rows.size() != 4) {
    return;
  }
  CHECK_EQ(std::get<std::string>(tables[0].rows[0][1]), std::string("a,b"));
  CHECK_EQ(std::get<std::string>(tables[0].rows[1][1]),
           std::string("say \"hi\""));
  CHECK_EQ(std::get<std::string>(tables[0].rows[2][1]), std::string("plain"));
  CHECK_EQ(std::get<std::string>(tables[0].rows[3][1]), std::string(""));
}

TEST(test_csv_load_empty_table_file_has_no_rows) {
  const char* const contents[] = {"", "id,username,location\n", "\n\n"};

  for (const char* content : contents) {
    TempDir dir;
    WriteFixtureWithUsers(dir.path(), content);
    auto repository = MakeCsvRepository(dir.path());

    std::vector<LoadedTable> tables = MustLoad(*repository);

    CHECK_EQ(tables.size(), std::size_t{3});
    CHECK_EQ(DescribeTable(tables, "users"), "");
  }
}

TEST(test_csv_load_reads_64_bit_integers) {
  TempDir dir;
  WriteFixtureC1(dir.path());
  WriteFile(dir.path() / "posts.csv",
            "id,content,username,views\n"
            "1,a,a,2147483648\n"
            "2,b,b,-9223372036854775808\n"
            "3,c,a,9223372036854775807\n");
  auto repository = MakeCsvRepository(dir.path());

  std::vector<LoadedTable> tables = MustLoad(*repository);

  CHECK_EQ(DescribeTable(tables, "posts"),
           "1,a,a,2147483648\n"
           "2,b,b,-9223372036854775808\n"
           "3,c,a,9223372036854775807\n");
}

TEST(test_csv_commit_writes_catalog_and_new_table_together) {
  TempDir dir;
  WriteFixtureA(dir.path());
  auto repository = MakeCsvRepository(dir.path());
  CommitSet set;
  set.catalog = std::vector<domain::TableSchema>{
      UsersSchema(), PostsSchema(), EngagementsSchema(), NotesSchema()};
  set.writes = {TableWrite{NotesSchema(), {}, true}};

  std::optional<std::string> failure = repository->Commit(set);

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  auto parsed =
      infrastructure::ParseCatalog(ReadFile(dir.path() / "catalog.csv"));
  const auto* schemas = std::get_if<std::vector<domain::TableSchema>>(&parsed);
  CHECK(schemas != nullptr);
  if (schemas != nullptr && schemas->size() == 4) {
    CHECK_EQ((*schemas)[3].name, std::string("notes"));
    CHECK_EQ((*schemas)[3].columns.size(), std::size_t{2});
  } else {
    CHECK(false);
  }
  CHECK_EQ(ReadFile(dir.path() / "notes.csv"), "id,body\n");
  CHECK_EQ(ListDirectory(dir.path()),
           "catalog.csv,engagements.csv,notes.csv,posts.csv,users.csv");
}

TEST(test_csv_commit_drops_table_file_and_catalog_entry) {
  TempDir dir;
  WriteFixtureA(dir.path());
  WriteFile(dir.path() / "catalog.csv",
            infrastructure::SerializeCatalog({UsersSchema(), PostsSchema(),
                                              EngagementsSchema(),
                                              MeasuresSchema()}));
  WriteFile(dir.path() / "measures.csv", "id,score\n1,2.5\n");
  auto repository = MakeCsvRepository(dir.path());
  CommitSet set;
  set.catalog = std::vector<domain::TableSchema>{UsersSchema(), PostsSchema(),
                                                 EngagementsSchema()};
  set.drops = {"measures"};

  std::optional<std::string> failure = repository->Commit(set);

  CHECK_EQ(DescribeOutcome(failure), kSuccess);
  CHECK_EQ(ListDirectory(dir.path()), kFixtureAListing);
  CHECK(ReadFile(dir.path() / "catalog.csv").find("measures") ==
        std::string::npos);
}

TEST(test_csv_commit_regenerates_header_only_when_requested) {
  TempDir dir;
  WriteFile(dir.path() / "users.csv", "ID,UserName,Location\n1,alice,Paris\n");
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> kept = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "alice", "Lyon")}, false}});
  const std::string kept_text = ReadFile(dir.path() / "users.csv");
  std::optional<std::string> regenerated = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "alice", "Lyon")}, true}});

  CHECK_EQ(DescribeOutcome(kept), kSuccess);
  CHECK_EQ(DescribeOutcome(regenerated), kSuccess);
  CHECK_EQ(kept_text, "ID,UserName,Location\n1,alice,Lyon\n");
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           "id,username,location\n1,alice,Lyon\n");
}

TEST(test_csv_commit_failure_restores_catalog_and_created_file) {
  TempDir dir;
  WriteFixtureA(dir.path());
  const std::string catalog_before = ReadFile(dir.path() / "catalog.csv");
  MakeUnremovableDirectory(dir.path() / "notes.csv.temp");
  auto repository = MakeCsvRepository(dir.path());
  CommitSet set;
  set.catalog = std::vector<domain::TableSchema>{
      UsersSchema(), PostsSchema(), EngagementsSchema(), NotesSchema()};
  set.writes = {TableWrite{NotesSchema(), {}, true}};

  std::optional<std::string> failure = repository->Commit(set);

  CHECK_EQ(DescribeOutcome(failure), "could not write notes.csv");
  CHECK_EQ(ReadFile(dir.path() / "catalog.csv"), catalog_before);
  CHECK_EQ(ListDirectory(dir.path()),
           "catalog.csv,engagements.csv,notes.csv.temp,posts.csv,users.csv");
}

TEST(test_csv_commit_and_append_quote_fields_that_need_it) {
  TempDir dir;
  WriteFixtureA(dir.path());
  auto repository = MakeCsvRepository(dir.path());

  std::optional<std::string> committed = CommitWrites(
      *repository,
      {TableWrite{UsersSchema(), {UserRow(1, "a,b", "say \"hi\"")}}});
  std::optional<std::string> appended =
      repository->AppendRows(UsersSchema(), {UserRow(2, "plain", "x,y")});
  std::vector<LoadedTable> tables = MustLoad(*repository);

  CHECK_EQ(DescribeOutcome(committed), kSuccess);
  CHECK_EQ(DescribeOutcome(appended), kSuccess);
  CHECK_EQ(ReadFile(dir.path() / "users.csv"),
           "id,username,location\n"
           "1,\"a,b\",\"say \"\"hi\"\"\"\n"
           "2,plain,\"x,y\"\n");
  CHECK_EQ(DescribeTable(tables, "users"),
           "1,\"a,b\",\"say \"\"hi\"\"\"\n2,plain,\"x,y\"\n");
}

TEST(test_csv_inspect_table_reports_absent_present_and_errors) {
  TempDir dir;
  WriteFixtureA(dir.path());
  auto repository = MakeCsvRepository(dir.path());
  const domain::TableSchema measures = MeasuresSchema();

  infrastructure::InspectOutcome absent = repository->InspectTable(measures);
  infrastructure::InspectOutcome present =
      repository->InspectTable(EngagementsSchema());
  WriteFile(dir.path() / "measures.csv", "");
  infrastructure::InspectOutcome empty = repository->InspectTable(measures);
  WriteFile(dir.path() / "measures.csv", "id\n1\n");
  infrastructure::InspectOutcome mismatch = repository->InspectTable(measures);
  std::error_code error;
  std::filesystem::remove(dir.path() / "measures.csv", error);
  std::filesystem::create_directory(dir.path() / "measures.csv", error);
  infrastructure::InspectOutcome directory = repository->InspectTable(measures);

  using Rows = std::optional<std::vector<domain::Row>>;
  const Rows* absent_rows = std::get_if<Rows>(&absent);
  const Rows* present_rows = std::get_if<Rows>(&present);
  const Rows* empty_rows = std::get_if<Rows>(&empty);
  CHECK(absent_rows != nullptr && !absent_rows->has_value());
  CHECK(present_rows != nullptr && present_rows->has_value());
  if (present_rows != nullptr && present_rows->has_value()) {
    CHECK_EQ((*present_rows)->size(), std::size_t{5});
  }
  CHECK(empty_rows != nullptr && empty_rows->has_value());
  if (empty_rows != nullptr && empty_rows->has_value()) {
    CHECK((*empty_rows)->empty());
  }
  const std::string* mismatch_text = std::get_if<std::string>(&mismatch);
  const std::string* directory_text = std::get_if<std::string>(&directory);
  CHECK(mismatch_text != nullptr);
  CHECK(directory_text != nullptr);
  if (mismatch_text != nullptr && directory_text != nullptr) {
    CHECK_EQ(*mismatch_text,
             "header of measures.csv does not match the declared columns");
    CHECK_EQ(*directory_text, "not a regular file: measures.csv");
  }
}

TEST(test_csv_append_rows_writes_every_row_in_one_call) {
  // Existing file with data: three rows follow the last line.
  TempDir with_data;
  WriteFixtureC1(with_data.path());
  auto repository = MakeCsvRepository(with_data.path());
  const std::string before = ReadFile(with_data.path() / "users.csv");
  std::optional<std::string> appended = repository->AppendRows(
      UsersSchema(), {UserRow(5, "eve", "Rome"), UserRow(6, "frank", "x,y"),
                      UserRow(7, "gina", "Oslo")});
  CHECK_EQ(DescribeOutcome(appended), kSuccess);
  CHECK_EQ(ReadFile(with_data.path() / "users.csv"),
           before + "5,eve,Rome\n6,frank,\"x,y\"\n7,gina,Oslo\n");

  // Absent file: the header comes first, then both rows.
  TempDir absent;
  WriteCatalogFixture(absent.path());
  auto absent_repository = MakeCsvRepository(absent.path());
  CHECK_EQ(DescribeOutcome(absent_repository->AppendRows(
               UsersSchema(), {UserRow(1, "a", "x"), UserRow(2, "b", "y")})),
           kSuccess);
  CHECK_EQ(ReadFile(absent.path() / "users.csv"),
           std::string("id,username,location\n1,a,x\n2,b,y\n"));

  // A file with \r\n line endings keeps them for every new row.
  TempDir crlf;
  WriteCatalogFixture(crlf.path());
  WriteFile(crlf.path() / "users.csv", "id,username,location\r\n1,a,x\r\n");
  auto crlf_repository = MakeCsvRepository(crlf.path());
  CHECK_EQ(DescribeOutcome(crlf_repository->AppendRows(
               UsersSchema(), {UserRow(2, "b", "y"), UserRow(3, "c", "z")})),
           kSuccess);
  CHECK_EQ(ReadFile(crlf.path() / "users.csv"),
           std::string("id,username,location\r\n1,a,x\r\n2,b,y\r\n3,c,z\r\n"));

  // An empty list succeeds and touches nothing, not even to create the file.
  TempDir none;
  WriteCatalogFixture(none.path());
  auto none_repository = MakeCsvRepository(none.path());
  CHECK_EQ(DescribeOutcome(none_repository->AppendRows(UsersSchema(), {})),
           kSuccess);
  CHECK(!std::filesystem::exists(none.path() / "users.csv"));
}

TEST(test_csv_load_reports_rejected_line_numbers) {
  TempDir rejecting;
  WriteFixtureB(rejecting.path());
  TempDir clean;
  WriteFixtureA(clean.path());

  std::vector<LoadedTable> rejected =
      MustLoad(*MakeCsvRepository(rejecting.path()));
  std::vector<LoadedTable> accepted =
      MustLoad(*MakeCsvRepository(clean.path()));

  CHECK_EQ(rejected.size(), std::size_t{3});
  if (rejected.size() == 3) {
    CHECK_EQ(rejected[0].rejected_lines, std::vector<std::size_t>({3, 4}));
    CHECK_EQ(rejected[1].rejected_lines, std::vector<std::size_t>({3}));
    CHECK(rejected[2].rejected_lines.empty());
  }
  for (const LoadedTable& table : accepted) {
    CHECK(table.rejected_lines.empty());
  }
}

}  // namespace flatfiledb::testing
