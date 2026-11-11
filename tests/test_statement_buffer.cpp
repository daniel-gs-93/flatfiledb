#include <string>
#include <vector>

#include "flatfiledb/statement_buffer.h"
#include "test_harness.h"

// Unit tier: StatementBuffer is pure, so each test feeds lines and reads back
// the statements (docs/TestDocument.md section 3.1, UT-82 to UT-85).

namespace {

namespace cli = flatfiledb::cli;

using Statements = std::vector<std::string>;

}  // namespace

TEST(test_statement_buffer_splits_statements_at_semicolons) {
  cli::StatementBuffer single;
  CHECK_EQ(single.AddLine("SELECT 1;"), Statements({"SELECT 1;"}));
  CHECK(single.IsEmpty());

  cli::StatementBuffer two;
  CHECK_EQ(two.AddLine("SELECT 1; SELECT 2;"),
           Statements({"SELECT 1;", " SELECT 2;"}));
  CHECK(two.IsEmpty());

  cli::StatementBuffer open_second;
  CHECK_EQ(open_second.AddLine("SELECT 1; SELECT 2"),
           Statements({"SELECT 1;"}));
  CHECK(!open_second.IsEmpty());

  cli::StatementBuffer padded;
  CHECK_EQ(padded.AddLine("  SELECT 3;  "), Statements({"  SELECT 3;"}));
  CHECK(padded.IsEmpty());
}

TEST(test_statement_buffer_keeps_semicolons_inside_string_literals) {
  const Statements inputs = {"INSERT INTO t VALUES ('a;b');",
                             "SELECT 'it''s; ok';", "SELECT '';",
                             "SELECT ';';"};
  for (const std::string& input : inputs) {
    cli::StatementBuffer buffer;
    CHECK_EQ(buffer.AddLine(input), Statements({input}));
    CHECK(buffer.IsEmpty());
  }
}

TEST(test_statement_buffer_joins_lines_until_semicolon) {
  cli::StatementBuffer table;
  CHECK_EQ(table.AddLine("CREATE TABLE t ("), Statements());
  CHECK(!table.IsEmpty());
  CHECK_EQ(table.AddLine("  id INT"), Statements());
  CHECK(!table.IsEmpty());
  CHECK_EQ(table.AddLine(");"), Statements({"CREATE TABLE t (\n  id INT\n);"}));
  CHECK(table.IsEmpty());

  cli::StatementBuffer literal;
  CHECK_EQ(literal.AddLine("SELECT 'a"), Statements());
  CHECK(!literal.IsEmpty());
  CHECK_EQ(literal.AddLine("b;c"), Statements());
  CHECK(!literal.IsEmpty());
  CHECK_EQ(literal.AddLine("d';"), Statements({"SELECT 'a\nb;c\nd';"}));
  CHECK(literal.IsEmpty());
}

TEST(test_statement_buffer_ignores_empty_statements) {
  const Statements inputs = {";", ";;", "; ;", "", "   "};
  for (const std::string& input : inputs) {
    cli::StatementBuffer buffer;
    CHECK_EQ(buffer.AddLine(input), Statements());
    CHECK(buffer.IsEmpty());
  }
}
