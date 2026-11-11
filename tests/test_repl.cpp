#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "flatfiledb/repl.h"
#include "flatfiledb/sql_parser.h"
#include "test_support.h"

// Unit and Integration tiers for the REPL (docs/TestDocument.md sections 3.1
// and 3.2, UT-86, UT-87, IT-98 to IT-107): RunRepl runs over string streams, so
// statement splitting, commands, prompts and `.schema` are checked without a
// terminal.

namespace {

namespace adapter = flatfiledb::adapter;
namespace cli = flatfiledb::cli;
namespace domain = flatfiledb::domain;
namespace testing = flatfiledb::testing;

using Lines = std::vector<std::string>;

const char* const kNoDatabaseLine = "error: no database open; run .open first";
const char* const kFixtureALoaded = "loaded users=4 posts=3 engagements=5";
const char* const kIncompleteLine =
    "error: incomplete statement at end of input";

const char* const kUsersCreate =
    "CREATE TABLE users (\n"
    "  id INT PRIMARY KEY,\n"
    "  username TEXT UNIQUE,\n"
    "  location TEXT\n"
    ");";
const char* const kPostsCreate =
    "CREATE TABLE posts (\n"
    "  id INT PRIMARY KEY,\n"
    "  content TEXT,\n"
    "  username TEXT REFERENCES users(username),\n"
    "  views INT\n"
    ");";
const char* const kEngagementsCreate =
    "CREATE TABLE engagements (\n"
    "  id INT PRIMARY KEY,\n"
    "  postId INT REFERENCES posts(id),\n"
    "  username TEXT REFERENCES users(username),\n"
    "  type TEXT,\n"
    "  comment TEXT,\n"
    "  timestamp INT\n"
    ");";

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

// Runs the REPL over input_lines and returns everything it wrote.
std::string RunText(const Lines& input_lines, bool is_interactive = false) {
  std::istringstream input(Joined(input_lines));
  std::ostringstream output;
  cli::RunRepl(input, output, is_interactive);
  return output.str();
}

Lines Run(const Lines& input_lines) { return SplitLines(RunText(input_lines)); }

std::string OpenLine(const std::filesystem::path& directory) {
  return ".open " + directory.string();
}

std::string ParseErrorText(const std::string& statement, std::size_t column) {
  return cli::FormatParseError(statement, adapter::ParseError{column, "d"});
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.starts_with(prefix);
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

std::vector<domain::TableSchema> FakeSchemas() {
  testing::FakeCatalog catalog;
  std::vector<domain::TableSchema> schemas;
  for (const std::string& name : catalog.TableNames()) {
    schemas.push_back(*catalog.FindTable(name));
  }
  return schemas;
}

const domain::TableSchema& SchemaNamed(
    const std::vector<domain::TableSchema>& schemas, const std::string& name) {
  for (const domain::TableSchema& schema : schemas) {
    if (schema.name == name) {
      return schema;
    }
  }
  return schemas.front();
}

}  // namespace

TEST(test_repl_parse_error_position_is_reported_per_line) {
  CHECK_EQ(ParseErrorText("SELEC x;", 1),
           std::string("syntax error at column 1: d"));
  CHECK_EQ(ParseErrorText("SELECT FROM\nt;", 8),
           std::string("syntax error at column 8: d"));
  CHECK_EQ(ParseErrorText("SELECT a\nFROM ;", 15),
           std::string("syntax error at line 2, column 6: d"));
  CHECK_EQ(ParseErrorText("SELECT a\nFROM", 14),
           std::string("syntax error at line 2, column 5: d"));
}

TEST(test_repl_format_create_table_renders_every_constraint) {
  const std::vector<domain::TableSchema> schemas = FakeSchemas();

  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "users"), schemas),
           std::string(kUsersCreate));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "posts"), schemas),
           std::string(kPostsCreate));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "drafts"), schemas),
           std::string("CREATE TABLE drafts (\n"
                       "  id INT PRIMARY KEY,\n"
                       "  body TEXT,\n"
                       "  owner TEXT DEFAULT 'none',\n"
                       "  score DOUBLE DEFAULT 1.0\n"
                       ");"));
  CHECK_EQ(cli::FormatCreateTable(SchemaNamed(schemas, "jobs"), schemas),
           std::string("CREATE TABLE jobs (\n"
                       "  id INT PRIMARY KEY AUTOINCREMENT,\n"
                       "  body TEXT\n"
                       ");"));

  domain::ColumnDef note{"note", domain::DataType::kText,
                         domain::Value(std::string("it's"))};
  domain::TableSchema quoted{
      "quoted", {domain::ColumnDef{"id", domain::DataType::kInt}, note}, 0, {}};
  CHECK_EQ(cli::FormatCreateTable(quoted, {quoted}),
           std::string("CREATE TABLE quoted (\n"
                       "  id INT PRIMARY KEY,\n"
                       "  note TEXT DEFAULT 'it''s'\n"
                       ");"));
}

TEST(test_repl_multi_line_statement_executes_once_complete) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output = Run(
      {OpenLine(dir.path()), "SELECT username", "FROM users", "WHERE id = 1;"});

  CHECK_EQ(output, Lines({kFixtureALoaded, "username", "alice"}));
}

TEST(test_repl_incomplete_statement_at_end_of_input_reports_error) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines unfinished = Run({OpenLine(dir.path()), "SELECT 1"});
  Lines open_literal = Run({OpenLine(dir.path()), "SELECT 'a;"});

  CHECK_EQ(unfinished, Lines({kFixtureALoaded, kIncompleteLine}));
  CHECK_EQ(open_literal, Lines({kFixtureALoaded, kIncompleteLine}));
}

TEST(test_repl_commands_start_only_at_the_beginning_of_a_statement) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines output =
      Run({OpenLine(dir.path()), "SELECT 1", ".quit", ";", ".tables"});

  CHECK_EQ(output.size(), std::size_t{5});
  if (output.size() != 5) {
    return;
  }
  CHECK_EQ(output[0], std::string(kFixtureALoaded));
  CHECK(StartsWith(output[1], "syntax error at line 2, column 1: "));
  CHECK_EQ(Lines(output.begin() + 2, output.end()),
           Lines({"users", "posts", "engagements"}));
}

TEST(test_repl_tables_lists_tables_in_catalog_order) {
  testing::TempDir fixture;
  testing::WriteFixtureA(fixture.path());
  testing::TempDir empty;

  Lines with_tables = Run({OpenLine(fixture.path()), ".tables"});
  Lines without_tables = Run({OpenLine(empty.path()), ".tables"});

  CHECK_EQ(with_tables,
           Lines({kFixtureALoaded, "users", "posts", "engagements"}));
  CHECK_EQ(without_tables, Lines({"loaded no tables"}));
}

TEST(test_repl_schema_prints_executable_statements) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());

  Lines lower = Run({OpenLine(dir.path()), ".schema posts"});
  Lines upper = Run({OpenLine(dir.path()), ".schema POSTS"});
  Lines every = Run({OpenLine(dir.path()), ".schema"});
  Lines missing = Run({OpenLine(dir.path()), ".schema nosuch"});

  const Lines posts_lines = SplitLines(std::string(kPostsCreate) + "\n");
  Lines expected_posts = {kFixtureALoaded};
  expected_posts.insert(expected_posts.end(), posts_lines.begin(),
                        posts_lines.end());
  CHECK_EQ(lower, expected_posts);
  CHECK_EQ(upper, expected_posts);

  Lines expected_every = {kFixtureALoaded};
  for (const char* text : {kUsersCreate, kPostsCreate, kEngagementsCreate}) {
    const Lines table_lines = SplitLines(std::string(text) + "\n");
    expected_every.insert(expected_every.end(), table_lines.begin(),
                          table_lines.end());
  }
  CHECK_EQ(every, expected_every);
  CHECK_EQ(missing, Lines({kFixtureALoaded, "error: no such table: nosuch"}));
}

TEST(test_repl_schema_round_trip_recreates_the_same_schema) {
  testing::TempDir original;
  testing::WriteFixtureA(original.path());
  Run({OpenLine(original.path()),
       "CREATE TABLE jobs (id INT PRIMARY KEY AUTOINCREMENT, title TEXT UNIQUE "
       "DEFAULT 'it''s; new', owner TEXT REFERENCES users(username), ratio "
       "DOUBLE DEFAULT 1.5);"});
  Lines first = Run({OpenLine(original.path()), ".schema"});
  CHECK(!first.empty());
  if (first.empty()) {
    return;
  }
  first.erase(first.begin());  // the `loaded ...` line

  testing::TempDir fresh;
  Lines replay = {OpenLine(fresh.path())};
  replay.insert(replay.end(), first.begin(), first.end());
  replay.push_back(".schema");
  Lines second = Run(replay);

  CHECK_EQ(second.size(), 1 + 4 + first.size());
  if (second.size() != 1 + 4 + first.size()) {
    return;
  }
  CHECK_EQ(second[0], std::string("loaded no tables"));
  CHECK_EQ(second[1], std::string("OK CREATE TABLE users"));
  CHECK_EQ(second[4], std::string("OK CREATE TABLE jobs"));
  CHECK_EQ(Lines(second.begin() + 5, second.end()), first);
}

TEST(test_repl_prompts_are_printed_only_when_interactive) {
  testing::TempDir dir;
  testing::WriteFixtureA(dir.path());
  const Lines input = {OpenLine(dir.path()), "SELECT username",
                       "FROM users WHERE id = 1;"};

  std::string plain = RunText(input, false);
  std::string prompted = RunText(input, true);

  CHECK(plain.find("flatfiledb>") == std::string::npos);
  CHECK_EQ(plain, std::string(kFixtureALoaded) + "\nusername\nalice\n");
  CHECK_EQ(prompted, std::string("flatfiledb> ") + kFixtureALoaded +
                         "\nflatfiledb>        ...> username\nalice\n"
                         "flatfiledb> ");
}

TEST(test_repl_help_lists_commands_and_quit_stops_reading) {
  Lines quit_first = Run({".help", ".quit", ".help"});
  Lines exit_first = Run({".exit", ".help"});

  CHECK(!quit_first.empty());
  for (const char* marker : {".open", ".tables", ".schema", ".help", ".quit"}) {
    CHECK_EQ(CountLinesStartingWith(quit_first, marker), 1);
  }
  Lines once = Run({".help"});
  CHECK_EQ(quit_first, once);
  CHECK_EQ(exit_first, Lines());
}

TEST(test_repl_commands_without_open_print_error) {
  Lines output = Run({".tables", ".schema", ".schema users"});

  CHECK_EQ(output, Lines({kNoDatabaseLine, kNoDatabaseLine, kNoDatabaseLine}));
}

TEST(test_repl_command_with_extra_arguments_prints_usage) {
  Lines output =
      Run({".tables x", ".schema a b", ".help x", ".quit x", ".help"});

  CHECK(output.size() > std::size_t{4});
  if (output.size() <= 4) {
    return;
  }
  CHECK_EQ(Lines(output.begin(), output.begin() + 4),
           Lines({"syntax error at column 1: usage: .tables",
                  "syntax error at column 1: usage: .schema [<table>]",
                  "syntax error at column 1: usage: .help",
                  "syntax error at column 1: usage: .quit"}));
  CHECK_EQ(Lines(output.begin() + 4, output.end()), Run({".help"}));
}

TEST(test_repl_commands_without_open_still_parse_sql_first) {
  Lines output = Run({"SELEC 1;", "SELECT username FROM users;"});

  CHECK_EQ(output.size(), std::size_t{2});
  if (output.size() != 2) {
    return;
  }
  CHECK(StartsWith(output[0], "syntax error at column 1: "));
  CHECK_EQ(output[1], std::string(kNoDatabaseLine));
}
