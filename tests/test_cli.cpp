#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "flatfiledb/directory_lock.h"
#include "test_support.h"

#ifndef FLATFILEDB_SOURCE_DIR
#error "FLATFILEDB_SOURCE_DIR must be defined by the build"
#endif
#ifndef FLATFILEDB_CLI_PATH
#error "FLATFILEDB_CLI_PATH must be defined by the build"
#endif

// Environment tier: runs the flatfiledb_cli executable against a copy of the
// real data/ directory. Each test owns a work directory containing data/.

namespace {

namespace testing = flatfiledb::testing;

const char* const kDataFiles[] = {"users.csv", "posts.csv", "engagements.csv"};
const char* const kOpenLine = ".open data";
const char* const kLoadedLine =
    "loaded users=10000 posts=10000 engagements=10000";

std::filesystem::path SourceData() {
  return std::filesystem::path(FLATFILEDB_SOURCE_DIR) / "data";
}

bool HasSourceData() {
  for (const char* name : kDataFiles) {
    std::error_code error;
    if (!std::filesystem::exists(SourceData() / name, error)) {
      return false;
    }
  }
  return true;
}

// Copies data/ to work_dir/data and checks that the copy matches byte for byte.
// should_write_catalog adds the catalog.csv of the `data/` example, so that
// `.open data` loads the three tables; without it the copy is a directory of
// bare CSV files.
bool PrepareWorkDir(const std::filesystem::path& work_dir,
                    bool should_write_catalog = true) {
  std::error_code error;
  std::filesystem::copy(SourceData(), work_dir / "data",
                        std::filesystem::copy_options::recursive, error);
  if (error) {
    return false;
  }
  for (const char* name : kDataFiles) {
    if (testing::ReadFile(SourceData() / name) !=
        testing::ReadFile(work_dir / "data" / name)) {
      return false;
    }
  }
  if (should_write_catalog) {
    testing::WriteCatalogFixture(work_dir / "data");
  }
  return true;
}

// Whether the three data files in work_dir/data are still the bytes of data/.
bool DataFilesUnchanged(const std::filesystem::path& work_dir) {
  for (const char* name : kDataFiles) {
    if (testing::ReadFile(SourceData() / name) !=
        testing::ReadFile(work_dir / "data" / name)) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

// Feeds input_lines to the CLI on stdin with work_dir as current directory and
// returns what it printed.
std::vector<std::string> RunCli(const std::filesystem::path& work_dir,
                                const std::vector<std::string>& input_lines) {
  std::string input;
  for (const auto& line : input_lines) {
    input += line + "\n";
  }
  testing::WriteFile(work_dir / "input.txt", input);
  std::string command = "cd '" + work_dir.string() + "' && '" +
                        FLATFILEDB_CLI_PATH + "' < input.txt > output.txt 2>&1";
  int status = std::system(command.c_str());
  if (status != 0) {
    flatfiledb::testing::ReportFailure(
        __FILE__, __LINE__, "cli exited with status " + std::to_string(status));
  }
  return SplitLines(testing::ReadFile(work_dir / "output.txt"));
}

// Output lines after the `loaded ...` line of the leading .open.
std::vector<std::string> RunAfterOpen(const std::filesystem::path& work_dir,
                                      std::vector<std::string> statements) {
  std::vector<std::string> input = {kOpenLine};
  for (auto& statement : statements) {
    input.push_back(std::move(statement));
  }
  std::vector<std::string> output = RunCli(work_dir, input);
  if (output.empty()) {
    return output;
  }
  output.erase(output.begin());
  return output;
}

std::vector<std::string> Sorted(std::vector<std::string> lines) {
  std::ranges::sort(lines);
  return lines;
}

const char* const kCommentQuery =
    "SELECT e.postId, e.comment FROM engagements e JOIN users u ON e.username "
    "= u.username "
    "WHERE u.id = 5805 AND e.type = 'comment' ORDER BY e.postId, e.comment;";
const char* const kLocationQuery =
    "SELECT e.type, COUNT(*) FROM engagements e JOIN users u ON e.username = "
    "u.username "
    "WHERE u.location = 'Patriciashire' GROUP BY e.type;";
const char* const kLocationFilteredQuery =
    "SELECT e.type, COUNT(*) FROM engagements e JOIN users u ON e.username = "
    "u.username "
    "WHERE u.location = 'Patriciashire' AND (e.type = 'like' OR e.type = "
    "'comment') "
    "GROUP BY e.type;";

}  // namespace

TEST(test_cli_open_prints_loaded_counts) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {std::string(kOpenLine) + " --parallel"});
  CHECK_EQ(output, std::vector<std::string>({kLoadedLine}));
}

TEST(test_cli_select_prints_header_and_rows) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(), {"SELECT username, location FROM users WHERE id = 1;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"username\tlocation", "knorman\tGrahamside"}));
}

TEST(test_cli_comment_query_prints_example_rows) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(work.path(), {kCommentQuery});
  CHECK_EQ(output, std::vector<std::string>(
                       {"postId\tcomment",
                        "7689\tOf lawyer rest design game large prove."}));
}

TEST(test_cli_location_query_prints_example_groups) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {kLocationFilteredQuery});
  CHECK_EQ(output.size(), std::size_t{3});
  if (output.size() != 3) {
    return;
  }
  CHECK_EQ(output[0], std::string("type\tCOUNT(*)"));
  CHECK_EQ(Sorted({output[1], output[2]}), Sorted({"like\t3", "comment\t1"}));
}

TEST(test_cli_empty_text_group_prints_empty_field) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(work.path(), {kLocationQuery});
  CHECK_EQ(output.size(), std::size_t{4});
  if (output.size() != 4) {
    return;
  }
  CHECK_EQ(output[0], std::string("type\tCOUNT(*)"));
  CHECK_EQ(Sorted({output[1], output[2], output[3]}),
           Sorted({"like\t3", "comment\t1", "\t1"}));
}

TEST(test_cli_write_statements_print_affected_rows) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(), {"INSERT INTO engagements (id, postId, username, type, "
                    "comment, timestamp) VALUES "
                    "(99999999, 1, 'ptaylor', 'like', 'None', 123456);",
                    "UPDATE posts SET views = views + 1 WHERE id = 1;",
                    "DELETE FROM engagements WHERE id = 99999999;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"OK affected=1", "OK affected=1", "OK affected=1"}));
}

TEST(test_cli_delete_referenced_user_prints_constraint_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"DELETE FROM users WHERE id = 5805;"});
  CHECK_EQ(output,
           std::vector<std::string>(
               {"error: constraint violation: users.id=5805 is referenced by "
                "posts.username"}));
}

TEST(test_cli_three_table_join_prints_unsupported) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELECT u.username FROM users u JOIN posts p "
                                 "ON p.username = u.username JOIN engagements "
                                 "e ON e.postId = p.id WHERE u.id = 5805;"});
  CHECK_EQ(output,
           std::vector<std::string>(
               {"error: unsupported: flatfile engine joins at most 2 tables"}));
}

TEST(test_cli_syntax_error_prints_column_and_description) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELEC username FROM users;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"syntax error at column 1: unrecognized statement"}));
}

TEST(test_cli_sql_before_open_prints_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunCli(work.path(), {"SELECT username FROM users WHERE id = 1;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"error: no database open; run .open first"}));
}

TEST(test_cli_failed_open_keeps_previous_connection) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunCli(
      work.path(),
      {kOpenLine, ".open nosuch", "SELECT username FROM users WHERE id = 1;"});
  CHECK_EQ(output,
           std::vector<std::string>(
               {kLoadedLine, "error: could not open nosuch: no such directory",
                "username", "knorman"}));
}

TEST(test_cli_formats_double_empty_result_and_blank_line) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(), {"SELECT id, id + 0.5, id + 1.0 FROM users WHERE id = 1;",
                    "", "SELECT id FROM users WHERE id = 0;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"id\tid + 0.5\tid + 1.0", "1\t1.5\t2.0", "id"}));
}

TEST(test_cli_bind_error_prints_bind_kind) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELECT nosuch FROM users;"});
  CHECK_EQ(output.size(), std::size_t{1});
  if (output.empty()) {
    return;
  }
  CHECK(output[0].starts_with("error: bind:"));
}

TEST(test_cli_write_failure_prints_io_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  if (::geteuid() == 0) {
    SKIP("running as root: directory permissions do not apply");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::error_code permission_error;
  std::filesystem::permissions(
      work.path() / "data",
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace, permission_error);
  CHECK(!permission_error);
  std::vector<std::string> output = RunAfterOpen(
      work.path(), {"UPDATE posts SET views = views + 1 WHERE id = 1;"});
  CHECK_EQ(output,
           std::vector<std::string>({"error: io: could not write posts.csv"}));
  CHECK(testing::ReadFile(work.path() / "data" / "posts.csv") ==
        testing::ReadFile(SourceData() / "posts.csv"));
}

TEST(test_cli_integer_overflow_prints_runtime_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id + 9223372036854775807 FROM users WHERE id = 1;"});
  CHECK_EQ(output,
           std::vector<std::string>({"error: runtime: numeric overflow"}));
}

TEST(test_cli_open_with_wrong_arguments_prints_usage) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunCli(work.path(), {".open"});
  CHECK_EQ(output, std::vector<std::string>(
                       {"syntax error at column 1: usage: .open <directory> "
                        "[--parallel]"}));
}

TEST(test_cli_create_tables_adopts_data_csvs) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> first = RunCli(
      work.path(),
      {".open data",
       "CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE, location "
       "TEXT);",
       "CREATE TABLE posts (id INT PRIMARY KEY, content TEXT, username TEXT "
       "REFERENCES users(username), views INT);",
       "CREATE TABLE engagements (id INT PRIMARY KEY, postId INT REFERENCES "
       "posts(id), username TEXT REFERENCES users(username), type TEXT, "
       "comment TEXT, timestamp INT);",
       "SELECT username, location FROM users WHERE id = 1;"});
  std::vector<std::string> second = RunCli(
      work.path(),
      {".open data", "SELECT username, location FROM users WHERE id = 1;"});

  CHECK_EQ(first, std::vector<std::string>(
                      {"loaded no tables",
                       "OK CREATE TABLE users (adopted 10000 rows)",
                       "OK CREATE TABLE posts (adopted 10000 rows)",
                       "OK CREATE TABLE engagements (adopted 10000 rows)",
                       "username\tlocation", "knorman\tGrahamside"}));
  CHECK_EQ(second, std::vector<std::string>({kLoadedLine, "username\tlocation",
                                             "knorman\tGrahamside"}));
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_ddl_statements_print_command_complete) {
  testing::TempDir work;
  std::filesystem::create_directory(work.path() / "db");

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open db",
       "CREATE TABLE notes (id INTEGER PRIMARY KEY AUTOINCREMENT, body TEXT "
       "NOT NULL, owner TEXT DEFAULT 'none');",
       "INSERT INTO notes (body) VALUES ('a, \"quoted\" text');",
       "ALTER TABLE notes ADD COLUMN pinned INT DEFAULT 0;",
       "SELECT id, body, owner, pinned FROM notes;", "DROP TABLE notes;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded no tables", "OK CREATE TABLE notes", "OK affected=1",
                "OK ALTER TABLE notes", "id\tbody\towner\tpinned",
                "1\ta, \"quoted\" text\tnone\t0", "OK DROP TABLE notes"}));
}

TEST(test_cli_ddl_errors_print_bind_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"CREATE TABLE users (id INT PRIMARY KEY);",
                                 "CREATE TABLE catalog (id INT PRIMARY KEY);",
                                 "DROP TABLE users;",
                                 "ALTER TABLE users ADD COLUMN x INT;"});

  CHECK_EQ(output.size(), std::size_t{4});
  if (output.size() != 4) {
    return;
  }
  CHECK_EQ(output[0], std::string("error: bind: table users already exists"));
  CHECK_EQ(output[1],
           std::string("error: bind: table name catalog is reserved"));
  CHECK_EQ(output[2], std::string("error: bind: table users is referenced by "
                                  "posts.username"));
  CHECK(output[3].starts_with("error: bind:"));
}

TEST(test_cli_open_locked_directory_prints_error) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> while_locked;
  {
    flatfiledb::infrastructure::LockOutcome held =
        flatfiledb::infrastructure::DirectoryLock::Acquire(
            (work.path() / "data").string());
    CHECK(held.lock != nullptr);
    while_locked = RunCli(work.path(), {kOpenLine});
  }

  std::vector<std::string> after_release = RunCli(work.path(), {kOpenLine});

  CHECK_EQ(while_locked,
           std::vector<std::string>(
               {"error: could not open data: database is locked by another "
                "process"}));
  CHECK_EQ(after_release, std::vector<std::string>({kLoadedLine}));
}

TEST(test_cli_open_same_directory_twice_succeeds) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunCli(
      work.path(),
      {kOpenLine, kOpenLine, "SELECT username FROM users WHERE id = 1;"});

  CHECK_EQ(output, std::vector<std::string>(
                       {kLoadedLine, kLoadedLine, "username", "knorman"}));
}

TEST(test_cli_text_with_comma_and_quote_round_trips) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> first = RunAfterOpen(
      work.path(),
      {"INSERT INTO posts (id, content, username, views) VALUES (99999999, "
       "'Hello, \"world\"', 'ptaylor', 1);",
       "SELECT content FROM posts WHERE id = 99999999;"});
  std::vector<std::string> second = RunAfterOpen(
      work.path(), {"SELECT content FROM posts WHERE id = 99999999;"});

  CHECK_EQ(first, std::vector<std::string>(
                      {"OK affected=1", "content", "Hello, \"world\""}));
  CHECK_EQ(second, std::vector<std::string>({"content", "Hello, \"world\""}));
}
