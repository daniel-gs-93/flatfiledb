#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "flatfiledb/repl.h"
#include "test_support.h"

#ifndef FLATFILEDB_CLI_PATH
#error "FLATFILEDB_CLI_PATH must be defined by the build"
#endif

// The `.engine` command, the engine setting of `.open`, the `note:` lines and
// the failure output of the paged engine (docs/PagedExecutionTestDocument.md).
// RunRepl runs over string streams on temporary directories; the documented
// demonstration of design document 3.4 runs the real flatfiledb_cli
// executable.

namespace {

namespace cli = flatfiledb::cli;
namespace testing = flatfiledb::testing;

using Lines = std::vector<std::string>;

const char* const kEngineUsageLine =
    "syntax error at column 1: usage: .engine [flatfile|paged]";
const char* const kNoteOfFlatFileCatalog =
    "note: this directory also has a flatfile catalog (catalog.csv); .engine "
    "flatfile opens it";
const char* const kNoteOfPagedFile =
    "note: this directory also has a paged database (paged.db); .engine paged "
    "opens it";

std::string Joined(const Lines& lines) {
  std::string text;
  for (const std::string& line : lines) {
    text += line + "\n";
  }
  return text;
}

Lines SplitLines(const std::string& text) {
  Lines lines;
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

// Runs the REPL over input_lines and returns the lines it wrote.
Lines Run(const Lines& input_lines) {
  std::istringstream input(Joined(input_lines));
  std::ostringstream output;
  cli::RunRepl(input, output, false);
  return SplitLines(output.str());
}

// How many lines start with marker followed by a space.
int CountLinesStartingWith(const Lines& lines, const std::string& marker) {
  int count = 0;
  for (const std::string& line : lines) {
    if (line.starts_with(marker + " ")) {
      ++count;
    }
  }
  return count;
}

std::string OpenLine(const std::filesystem::path& directory) {
  return ".open " + directory.string();
}

}  // namespace

TEST(test_engine_command_show_set_and_usage) {
  // Act
  const Lines output =
      Run({".engine", ".engine paged", ".engine", ".engine sqlite",
           ".engine paged extra", "engine paged", ".help"});

  // Assert: the six lines of the documented sequence, then the help text.
  CHECK(output.size() > std::size_t{6});
  if (output.size() <= 6) {
    return;
  }
  CHECK_EQ(Lines(output.begin(), output.begin() + 6),
           Lines({"engine flatfile", "engine paged", "engine paged",
                  kEngineUsageLine, kEngineUsageLine,
                  "error: commands start with '.'; did you mean .engine"}));
  const Lines help(output.begin() + 6, output.end());
  CHECK_EQ(help, Run({".help"}));
  CHECK_EQ(CountLinesStartingWith(help, ".engine"), 1);

  // The setting is not touched by an error, naming the same engine again only
  // prints it, and it is not kept from one run to the next.
  CHECK_EQ(
      Run({".engine paged", ".engine sqlite", ".engine paged extra", ".engine",
           ".engine paged", ".engine flatfile", ".engine"}),
      Lines({"engine paged", kEngineUsageLine, kEngineUsageLine, "engine paged",
             "engine paged", "engine flatfile", "engine flatfile"}));
  CHECK_EQ(Run({".engine"}), Lines({"engine flatfile"}));
  CHECK_EQ(Run({".engine flatfile"}), Lines({"engine flatfile"}));
}

namespace {

// Flat-file to paged fails because paged.db is not a database file, and paged
// to flat-file fails because a table file of the catalog is missing: each time
// the database and the engine setting stay as they were.
void CheckFailedSwitchKeepsDatabaseAndSetting() {
  const std::string flatfile_loaded = "loaded users=4 posts=3 engagements=5";

  // Flat-file to paged: paged.db is not a database file.
  testing::TempDir garbage_dir;
  testing::WriteFixtureA(garbage_dir.path());
  testing::WriteFile(garbage_dir.path() / "paged.db", "this is not a database");
  const std::string garbage_canonical =
      std::filesystem::canonical(garbage_dir.path()).string();
  const Lines to_paged =
      Run({OpenLine(garbage_dir.path()), ".engine paged", ".engine",
           "SELECT username FROM users WHERE id = 1;", ".engine flatfile"});

  // Paged to flat-file: a table file of the catalog is missing.
  testing::TempDir broken_dir;
  testing::WriteFixtureA(broken_dir.path());
  std::error_code error;
  std::filesystem::remove(broken_dir.path() / "posts.csv", error);
  CHECK(!error);
  const std::string broken_canonical =
      std::filesystem::canonical(broken_dir.path()).string();
  const Lines to_flatfile =
      Run({".engine paged", OpenLine(broken_dir.path()),
           "CREATE TABLE t (id INT PRIMARY KEY);", ".engine flatfile",
           ".engine", "INSERT INTO t VALUES (7);", "SELECT id FROM t;"});

  CHECK(to_paged.size() == std::size_t{6});
  if (to_paged.size() == 6) {
    CHECK_EQ(to_paged[0], flatfile_loaded);
    CHECK(to_paged[1].starts_with("error: could not open " + garbage_canonical +
                                  ": corrupt: "));
    CHECK_EQ(
        Lines(to_paged.begin() + 2, to_paged.end()),
        Lines({"engine flatfile", "username", "alice", "engine flatfile"}));
  }
  CHECK_EQ(to_flatfile, Lines({"engine paged", "loaded no tables",
                               kNoteOfFlatFileCatalog, "OK CREATE TABLE t",
                               "error: could not open " + broken_canonical +
                                   ": cannot read posts.csv",
                               "engine paged", "OK affected=1", "id", "7"}));
}

}  // namespace

TEST(test_engine_command_switch_reopens_directory) {
  testing::TempDir dir;
  const std::string directory = dir.path().string();
  const std::string missing = (dir.path() / "missing" / "child").string();

  // Act: the flat-file database is switched to paged and back; a failed open
  // and a --parallel open follow; rows are added to the paged database
  // without leaving, then the same directory is opened again.
  const Lines output = Run(
      {".open " + directory,
       "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT);",
       "INSERT INTO notes VALUES (1, 'flat');", ".engine paged",
       ".engine paged", "CREATE TABLE posts (id INT PRIMARY KEY, body TEXT);",
       ".engine flatfile", "SELECT body FROM notes;", ".engine paged",
       ".open " + missing, ".engine", "INSERT INTO posts VALUES (1, 'one');",
       ".open " + directory + " --parallel", "SELECT body FROM posts;",
       "INSERT INTO posts VALUES (2, 'two');"});
  const Lines reopened = Run({".engine paged", ".open " + directory,
                              "SELECT id, body FROM posts ORDER BY id;"});

  // Assert
  CHECK_EQ(output,
           Lines({"loaded no tables",
                  "OK CREATE TABLE notes",
                  "OK affected=1",
                  "engine paged",
                  "loaded no tables",
                  kNoteOfFlatFileCatalog,
                  "engine paged",
                  "OK CREATE TABLE posts",
                  "engine flatfile",
                  "loaded notes=1",
                  "body",
                  "flat",
                  "engine paged",
                  "loaded posts=0",
                  "error: could not open " + missing + ": no such directory",
                  "engine paged",
                  "OK affected=1",
                  "loaded posts=1",
                  "body",
                  "one",
                  "OK affected=1"}));
  CHECK_EQ(reopened, Lines({"engine paged", "loaded posts=2", "id\tbody",
                            "1\tone", "2\ttwo"}));
  CheckFailedSwitchKeepsDatabaseAndSetting();
}

TEST(test_engine_command_notes_for_other_engine_files) {
  // The directory has catalog.csv: paged opens it with no table.
  testing::TempDir catalog_dir;
  testing::WriteFixtureA(catalog_dir.path());
  const Lines paged_beside_catalog =
      Run({".engine paged", OpenLine(catalog_dir.path())});

  // The directory has a paged.db with tables and no catalog.csv: flat-file
  // opens it with no table.
  testing::TempDir paged_dir;
  Run({".engine paged", OpenLine(paged_dir.path()),
       "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT);"});
  const Lines flatfile_beside_paged_tables = Run({OpenLine(paged_dir.path())});

  // The directory has a paged.db without tables.
  testing::TempDir empty_paged_dir;
  Run({".engine paged", OpenLine(empty_paged_dir.path())});
  const Lines flatfile_beside_empty_paged =
      Run({OpenLine(empty_paged_dir.path())});

  // Neither file exists, for either engine.
  testing::TempDir plain_dir;
  testing::TempDir plain_paged_dir;
  const Lines flatfile_alone = Run({OpenLine(plain_dir.path())});
  const Lines paged_alone =
      Run({".engine paged", OpenLine(plain_paged_dir.path())});

  // Both files exist and the opened engine has tables.
  testing::TempDir both_dir;
  testing::WriteFixtureA(both_dir.path());
  Run({".engine paged", OpenLine(both_dir.path()),
       "CREATE TABLE notes (id INT PRIMARY KEY, body TEXT);"});
  const Lines paged_with_both =
      Run({".engine paged", OpenLine(both_dir.path())});
  const Lines flatfile_with_both = Run({OpenLine(both_dir.path())});

  // A directory the open creates has no other files at all.
  testing::TempDir parent;
  const Lines created =
      Run({".engine paged", OpenLine(parent.path() / "fresh")});

  CHECK_EQ(paged_beside_catalog,
           Lines({"engine paged", "loaded no tables", kNoteOfFlatFileCatalog}));
  CHECK_EQ(flatfile_beside_paged_tables,
           Lines({"loaded no tables", kNoteOfPagedFile}));
  CHECK_EQ(flatfile_beside_empty_paged,
           Lines({"loaded no tables", kNoteOfPagedFile}));
  CHECK_EQ(flatfile_alone, Lines({"loaded no tables"}));
  CHECK_EQ(paged_alone, Lines({"engine paged", "loaded no tables"}));
  CHECK_EQ(paged_with_both, Lines({"engine paged", "loaded notes=0"}));
  CHECK_EQ(flatfile_with_both, Lines({"loaded users=4 posts=3 engagements=5"}));
  CHECK_EQ(created,
           Lines({"engine paged", "created new database " +
                                      (parent.path() / "fresh").string()}));
}

namespace {

// What one run of the real CLI executable printed.
struct CliRun {
  Lines output;
  int status;
};

// Feeds input_lines to the CLI on stdin with work_dir as the current directory.
CliRun RunCli(const std::filesystem::path& work_dir, const Lines& input_lines) {
  testing::WriteFile(work_dir / "input.txt", Joined(input_lines));
  const std::string command = "cd '" + work_dir.string() + "' && '" +
                              FLATFILEDB_CLI_PATH +
                              "' < input.txt > output.txt 2> error.txt";
  const int raw_status = std::system(command.c_str());
  CliRun run;
  run.status = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
  run.output = SplitLines(testing::ReadFile(work_dir / "output.txt"));
  return run;
}

}  // namespace

TEST(test_engine_command_documented_demo_output) {
  testing::TempDir work;

  const CliRun first =
      RunCli(work.path(),
             {".engine paged", ".open demo",
              "CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE);",
              "INSERT INTO users VALUES (1, 'ann'), (2, 'bob');",
              "SELECT id, username FROM users ORDER BY id;"});
  const bool has_file_after_first_run =
      std::filesystem::is_regular_file(work.path() / "demo" / "paged.db");
  const CliRun second = RunCli(work.path(), {".engine paged", ".open demo",
                                             "SELECT COUNT(*) FROM users;"});

  CHECK_EQ(first.status, 0);
  CHECK_EQ(first.output, Lines({"engine paged", "created new database demo",
                                "OK CREATE TABLE users", "OK affected=2",
                                "id\tusername", "1\tann", "2\tbob"}));
  CHECK(has_file_after_first_run);
  CHECK_EQ(second.status, 0);
  CHECK_EQ(second.output,
           Lines({"engine paged", "loaded users=2", "COUNT(*)", "2"}));
}

TEST(test_engine_command_failure_outputs) {
  testing::TempDir dir;
  const std::string long_text(4100, 'x');

  const Lines output =
      Run({".engine paged", OpenLine(dir.path()),
           "CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE);",
           "INSERT INTO users VALUES (1, 'ann'), (2, 'bob');",
           "CREATE TABLE posts (id INT PRIMARY KEY, username TEXT REFERENCES "
           "users(username));",
           "INSERT INTO posts VALUES (1, 'ann');",
           "DELETE FROM users WHERE id = 1;",
           "ALTER TABLE users ADD COLUMN pinned INT DEFAULT 0;",
           "DROP TABLE posts;",
           "INSERT INTO users VALUES (3, '" + long_text + "');",
           "UPDATE users SET username = '" + long_text + "' WHERE id = 2;",
           "INSERT INTO users VALUES (1, 'zed');",
           "SELECT id, username FROM users ORDER BY id;"});

  CHECK_EQ(output,
           Lines({"engine paged", "loaded no tables", "OK CREATE TABLE users",
                  "OK affected=2", "OK CREATE TABLE posts", "OK affected=1",
                  "error: constraint violation: users.id=1 is referenced by "
                  "posts.username",
                  "error: unsupported: paged engine does not support ALTER "
                  "TABLE ADD COLUMN",
                  "error: unsupported: paged engine does not support DROP "
                  "TABLE",
                  "error: unsupported: paged engine cannot store a row longer "
                  "than 4060 bytes in users",
                  "error: unsupported: paged engine cannot store a row longer "
                  "than 4060 bytes in users",
                  "error: constraint violation: duplicate key: users.id=1",
                  "id\tusername", "1\tann", "2\tbob"}));
}
