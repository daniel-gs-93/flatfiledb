#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "strata/directory_lock.h"
#include "test_support.h"

#ifndef STRATA_SOURCE_DIR
#error "STRATA_SOURCE_DIR must be defined by the build"
#endif
#ifndef STRATA_CLI_PATH
#error "STRATA_CLI_PATH must be defined by the build"
#endif

// Environment tier: runs the strata_cli executable against a copy of the
// real data/ directory. Each test owns a work directory containing data/.

namespace {

namespace testing = strata::testing;

const char* const kDataFiles[] = {"users.csv", "posts.csv", "engagements.csv"};
const char* const kOpenLine = ".open data";
// data/users.csv has three rows the schema rejects (lines 12, 17 and 18), which
// `.open` reports.
const char* const kLoadedLine =
    "loaded users=10000 (rejected 3) posts=10000 engagements=10000";
const char* const kRejectedLine = "users.csv: rejected lines 12, 17, 18";

std::filesystem::path SourceData() {
  return std::filesystem::path(STRATA_SOURCE_DIR) / "data";
}

// The sample database that ships with the repository (README, Quick Start).
std::filesystem::path SampleDatabase() {
  return std::filesystem::path(STRATA_SOURCE_DIR) / "sample_my_db";
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
                        STRATA_CLI_PATH + "' < input.txt > output.txt 2>&1";
  int status = std::system(command.c_str());
  if (status != 0) {
    strata::testing::ReportFailure(
        __FILE__, __LINE__, "cli exited with status " + std::to_string(status));
  }
  return SplitLines(testing::ReadFile(work_dir / "output.txt"));
}

// What one run of the CLI with command-line arguments printed and returned.
struct CliRun {
  std::vector<std::string> output;
  std::string error_text;
  int status;
};

// Runs the CLI with `arguments` (already shell-quoted), stdin from input_lines
// and work_dir as the current directory.
CliRun RunCliWithArguments(const std::filesystem::path& work_dir,
                           const std::string& arguments,
                           const std::vector<std::string>& input_lines) {
  std::string input;
  for (const auto& line : input_lines) {
    input += line + "\n";
  }
  testing::WriteFile(work_dir / "input.txt", input);
  std::string command = "cd '" + work_dir.string() + "' && '" +
                        STRATA_CLI_PATH + "' " + arguments +
                        " < input.txt > output.txt 2> error.txt";
  int raw_status = std::system(command.c_str());
  CliRun run;
  run.status = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
  run.output = SplitLines(testing::ReadFile(work_dir / "output.txt"));
  run.error_text = testing::ReadFile(work_dir / "error.txt");
  return run;
}

// Output lines after the `loaded ...` line of the leading .open.
std::vector<std::string> RunAfterOpen(const std::filesystem::path& work_dir,
                                      std::vector<std::string> statements) {
  std::vector<std::string> input = {kOpenLine};
  for (auto& statement : statements) {
    input.push_back(std::move(statement));
  }
  std::vector<std::string> output = RunCli(work_dir, input);
  // The `loaded ...` line and the line that lists the rejected rows.
  const std::size_t opening_lines = std::min<std::size_t>(2, output.size());
  output.erase(output.begin(), output.begin() + opening_lines);
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
  CHECK_EQ(output, std::vector<std::string>({kLoadedLine, kRejectedLine}));
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

TEST(test_cli_three_table_join_prints_rows) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT u.username, COUNT(*) FROM users u JOIN posts p ON p.username = "
       "u.username JOIN engagements e ON e.postId = p.id WHERE u.id = 5715 "
       "GROUP BY u.username;"});
  CHECK_EQ(output,
           std::vector<std::string>({"username\tCOUNT(*)", "kbrown\t9"}));
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
  std::vector<std::string> output =
      RunCli(work.path(), {kOpenLine, ".open nosuch/db",
                           "SELECT username FROM users WHERE id = 1;"});
  CHECK_EQ(output, std::vector<std::string>(
                       {kLoadedLine, kRejectedLine,
                        "error: could not open nosuch/db: no such directory",
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
  CHECK_EQ(second, std::vector<std::string>({kLoadedLine, kRejectedLine,
                                             "username\tlocation",
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
    strata::infrastructure::LockOutcome held =
        strata::infrastructure::DirectoryLock::Acquire(
            (work.path() / "data").string());
    CHECK(held.lock != nullptr);
    while_locked = RunCli(work.path(), {kOpenLine});
  }

  std::vector<std::string> after_release = RunCli(work.path(), {kOpenLine});

  CHECK_EQ(while_locked,
           std::vector<std::string>(
               {"error: could not open data: database is locked by another "
                "process"}));
  CHECK_EQ(after_release,
           std::vector<std::string>({kLoadedLine, kRejectedLine}));
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

  CHECK_EQ(output,
           std::vector<std::string>({kLoadedLine, kRejectedLine, kLoadedLine,
                                     kRejectedLine, "username", "knorman"}));
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

TEST(test_cli_multi_line_statement_prints_result) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT username,", "  location", "FROM users", "WHERE id = 1;"});

  CHECK_EQ(output, std::vector<std::string>(
                       {"username\tlocation", "knorman\tGrahamside"}));
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line + "\n";
  }
  return text;
}

TEST(test_cli_schema_output_can_be_replayed) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));
  std::filesystem::create_directory(work.path() / "fresh");

  std::vector<std::string> original_schema =
      RunAfterOpen(work.path(), {".schema"});
  testing::WriteFile(work.path() / "schema.sql", JoinLines(original_schema));
  std::vector<std::string> replay_input = {".open fresh"};
  replay_input.insert(replay_input.end(), original_schema.begin(),
                      original_schema.end());
  replay_input.push_back(".schema");
  std::vector<std::string> replay = RunCli(work.path(), replay_input);

  const std::size_t table_count = 3;
  CHECK_EQ(replay.size(), 1 + table_count + original_schema.size());
  if (replay.size() != 1 + table_count + original_schema.size()) {
    return;
  }
  CHECK_EQ(replay[0], std::string("loaded no tables"));
  CHECK_EQ(replay[1], std::string("OK CREATE TABLE users"));
  CHECK_EQ(replay[2], std::string("OK CREATE TABLE posts"));
  CHECK_EQ(replay[3], std::string("OK CREATE TABLE engagements"));
  CHECK_EQ(
      std::vector<std::string>(replay.begin() + 1 + table_count, replay.end()),
      original_schema);
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_piped_input_prints_no_prompt) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output =
      RunAfterOpen(work.path(), {"SELECT username FROM users WHERE id = 1;"});
  std::string raw = testing::ReadFile(work.path() / "output.txt");

  CHECK_EQ(output, std::vector<std::string>({"username", "knorman"}));
  CHECK_EQ(raw, std::string(kLoadedLine) + "\n" + kRejectedLine +
                    "\nusername\nknorman\n");
  CHECK(raw.find("strata>") == std::string::npos);
}

TEST(test_cli_quit_stops_reading_with_status_zero) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  // RunCli reports a failure when the exit status is not 0.
  std::vector<std::string> output =
      RunCli(work.path(),
             {kOpenLine, ".quit", "SELECT username FROM users WHERE id = 1;"});

  CHECK_EQ(output, std::vector<std::string>({kLoadedLine, kRejectedLine}));
}

TEST(test_cli_limit_offset_prints_window) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id, username FROM users ORDER BY id LIMIT 3 OFFSET 1;"});

  CHECK_EQ(output, std::vector<std::string>({"id\tusername", "2\tthomas90",
                                             "3\tjblair", "4\tjohn04"}));
}

TEST(test_cli_aggregates_print_min_max_avg) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT COUNT(*), MIN(timestamp), MAX(timestamp), AVG(timestamp) FROM "
       "engagements;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"COUNT(*)\tMIN(timestamp)\tMAX(timestamp)\tAVG(timestamp)",
                "10000\t1690823929\t1722355167\t1706505303.6826"}));
}

TEST(test_cli_like_and_functions_print_example_rows) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT id, UPPER(username), LENGTH(username) FROM users WHERE "
       "username LIKE 'kn%' ORDER BY id LIMIT 2;"});

  CHECK_EQ(output,
           std::vector<std::string>({"id\tUPPER(username)\tLENGTH(username)",
                                     "1\tKNORMAN\t7", "66\tKNIGHTLAURA\t11"}));
}

TEST(test_cli_new_syntax_errors_print_messages) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path()));

  std::vector<std::string> output = RunAfterOpen(
      work.path(),
      {"SELECT 1 / 0 FROM users WHERE id = 1;",
       "SELECT AVG(id) FROM users WHERE id = 0;",
       "SELECT CAST(username AS INT) FROM users WHERE id = 1;",
       "SELECT id FROM users LIMIT -1;", "SELECT FOO(1) FROM users;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"error: runtime: division by zero",
                "error: runtime: AVG over no rows",
                "error: runtime: cannot cast 'knorman' to INT",
                "syntax error at column 28: LIMIT requires a non-negative "
                "integer literal",
                "syntax error at column 8: unknown function FOO"}));
}

TEST(test_cli_multi_row_insert_persists_and_fails_as_a_whole) {
  testing::TempDir work;
  std::filesystem::create_directory(work.path() / "db");

  std::vector<std::string> first = RunCli(
      work.path(),
      {".open db",
       "CREATE TABLE notes (id INT PRIMARY KEY AUTOINCREMENT, body TEXT);",
       "INSERT INTO notes (body) VALUES ('a'), ('b'), ('c');",
       "INSERT INTO notes (id, body) VALUES (10, 'x'), (10, 'y');"});
  std::vector<std::string> second = RunCli(
      work.path(), {".open db", "SELECT id, body FROM notes ORDER BY id;"});

  CHECK_EQ(first,
           std::vector<std::string>(
               {"loaded no tables", "OK CREATE TABLE notes", "OK affected=3",
                "error: constraint violation: duplicate key: "
                "notes.id=10"}));
  CHECK_EQ(second, std::vector<std::string>(
                       {"loaded notes=3", "id\tbody", "1\ta", "2\tb", "3\tc"}));
}

TEST(test_cli_sample_database_answers_readme_queries) {
  testing::TempDir work;
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open sample_my_db", ".tables", ".schema courses",
       "SELECT code, title, credits, capacity FROM courses ORDER BY code LIMIT "
       "4;",
       "SELECT code, COUNT(*) FROM courses JOIN enrollments ON course_id = "
       "courses.id WHERE semester = '2026-Spring' AND status = 'enrolled' "
       "GROUP BY code ORDER BY 2 DESC, 1 LIMIT 3;",
       "SELECT name, code, score FROM students JOIN enrollments ON student_id "
       "= students.id JOIN courses ON course_id = courses.id WHERE semester = "
       "'2025-Fall' AND status = 'completed' ORDER BY 3 DESC, 1 LIMIT 3;",
       "SELECT id, name, LENGTH(name), UPPER(major) FROM students WHERE name "
       "LIKE 'K%';"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "instructors",
                "students",
                "courses",
                "enrollments",
                "CREATE TABLE courses (",
                "  id INT PRIMARY KEY AUTOINCREMENT,",
                "  code TEXT UNIQUE,",
                "  title TEXT,",
                "  credits INT,",
                "  dept TEXT,",
                "  capacity INT,",
                "  instructor_id INT REFERENCES instructors(id)",
                ");",
                "code\ttitle\tcredits\tcapacity",
                "ART101\tArt Appreciation\t2\t10",
                "CS101\tIntro to Programming\t3\t6",
                "CS201\tData Structures\t3\t5",
                "CS301\tDatabase Systems\t3\t5",
                "code\tCOUNT(*)",
                "HIST101\t8",
                "STAT101\t8",
                "ECON101\t7",
                "name\tcode\tscore",
                "Ivy Chen\tCS301\t97",
                "Wendy Morgan\tHIST101\t97",
                "Frank Miller\tCS301\t95",
                "id\tname\tLENGTH(name)\tUPPER(major)",
                "1011\tKevin Anderson\t14\tCS"}));
}

TEST(test_cli_sample_database_constraints_and_update_work_on_a_copy) {
  testing::TempDir work;
  const std::string source_enrollments =
      testing::ReadFile(SampleDatabase() / "enrollments.csv");
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open sample_my_db",
       "SELECT id, student_id, course_id, status, score FROM enrollments "
       "WHERE id = 100;",
       "INSERT INTO enrollments (student_id, course_id, semester) VALUES "
       "(9999, 1, '2026-Spring');",
       "DELETE FROM students WHERE id = 1001;",
       "UPDATE enrollments SET status = 'completed', score = 90 WHERE id = "
       "100;",
       "SELECT id, student_id, course_id, status, score FROM enrollments "
       "WHERE id = 100;"});

  CHECK_EQ(output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "id\tstudent_id\tcourse_id\tstatus\tscore",
                "100\t1008\t3\tenrolled\t0",
                "error: constraint violation: no matching row: "
                "enrollments.student_id=9999 not found in students.id",
                "error: constraint violation: students.id=1001 is referenced "
                "by enrollments.student_id",
                "OK affected=1", "id\tstudent_id\tcourse_id\tstatus\tscore",
                "100\t1008\t3\tcompleted\t90"}));
  CHECK(testing::ReadFile(work.path() / "sample_my_db" / "enrollments.csv") !=
        source_enrollments);
  CHECK(testing::ReadFile(SampleDatabase() / "enrollments.csv") ==
        source_enrollments);
}

TEST(test_cli_database_path_argument_opens_the_database) {
  testing::TempDir work;
  std::error_code copy_error;
  std::filesystem::copy(SampleDatabase(), work.path() / "sample_my_db",
                        std::filesystem::copy_options::recursive, copy_error);
  CHECK(!copy_error);

  CliRun existing =
      RunCliWithArguments(work.path(), "sample_my_db", {".tables"});
  CliRun created =
      RunCliWithArguments(work.path(), "fresh_db --parallel", {".tables"});

  CHECK_EQ(existing.status, 0);
  CHECK_EQ(existing.output,
           std::vector<std::string>(
               {"loaded instructors=6 students=30 courses=12 enrollments=144",
                "instructors", "students", "courses", "enrollments"}));
  CHECK_EQ(created.status, 0);
  CHECK_EQ(created.output,
           std::vector<std::string>({"created new database fresh_db"}));
  CHECK(std::filesystem::is_directory(work.path() / "fresh_db"));
}

TEST(test_cli_bad_arguments_print_usage_and_exit_with_status_two) {
  testing::TempDir work;

  CliRun too_many = RunCliWithArguments(work.path(), "a b c", {".tables"});
  CliRun bad_option =
      RunCliWithArguments(work.path(), "a --bogus", {".tables"});
  CliRun option_first =
      RunCliWithArguments(work.path(), "--parallel", {".tables"});

  for (const CliRun* run : {&too_many, &bad_option, &option_first}) {
    CHECK_EQ(run->status, 2);
    CHECK_EQ(run->error_text,
             std::string("usage: strata_cli [<directory> [--parallel]]\n"));
    CHECK(run->output.empty());
  }
  CHECK(!std::filesystem::exists(work.path() / "a"));
}

TEST(test_cli_import_loads_posts_and_engagements) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> output =
      RunCli(work.path(),
             {".open fresh", ".import data/posts.csv",
              ".import data/engagements.csv", "SELECT COUNT(*) FROM posts;",
              "SELECT COUNT(*) FROM engagements;", ".schema posts"});

  CHECK_EQ(
      output,
      std::vector<std::string>(
          {"created new database fresh",
           "OK IMPORT posts (imported 10000 rows)",
           "OK IMPORT engagements (imported 10000 rows)", "COUNT(*)", "10000",
           "COUNT(*)", "10000", "CREATE TABLE posts (", "  id INT PRIMARY KEY,",
           "  content TEXT,", "  username TEXT,", "  views INT", ");"}));
  CHECK(DataFilesUnchanged(work.path()));
}

TEST(test_cli_import_users_needs_type_and_skip_options) {
  if (!HasSourceData()) {
    SKIP("data/ is not present");
  }
  testing::TempDir work;
  CHECK(PrepareWorkDir(work.path(), false));

  std::vector<std::string> output = RunCli(
      work.path(),
      {".open fresh", ".import data/users.csv",
       ".import data/users.csv users --pk id --type id=INT --skip-bad-rows",
       "SELECT COUNT(*) FROM users;",
       "SELECT username FROM users WHERE id = 1;"});

  CHECK_EQ(output, std::vector<std::string>(
                       {"created new database fresh",
                        "error: bind: line 17: expected 3 fields, found 2",
                        "OK IMPORT users (imported 10000 rows, skipped 3)",
                        "skipped line 12: id '1s0' is not INT",
                        "skipped line 17: expected 3 fields, found 2",
                        "skipped line 18: expected 3 fields, found 4",
                        "COUNT(*)", "10000", "username", "knorman"}));
  CHECK(DataFilesUnchanged(work.path()));
}
