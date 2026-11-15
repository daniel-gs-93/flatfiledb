#include <string>
#include <vector>

#include "strata/statement_buffer.h"
#include "test_harness.h"

// Unit tier: StatementBuffer is pure, so each test feeds lines and reads back
// the statements (docs/TestDocument.md section 3.1).

namespace {

namespace cli = strata::cli;

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

TEST(test_statement_buffer_cancel_discards_pending_and_reports_string_state) {
  cli::StatementBuffer in_string;
  CHECK_EQ(in_string.AddLine("SELECT 'a"), Statements());
  CHECK(in_string.IsInString());
  CHECK(!in_string.IsEmpty());
  in_string.Cancel();
  CHECK(!in_string.IsInString());
  CHECK(in_string.IsEmpty());

  cli::StatementBuffer pending;
  CHECK_EQ(pending.AddLine("SELECT 1"), Statements());
  CHECK(!pending.IsInString());
  CHECK(!pending.IsEmpty());
  pending.Cancel();
  CHECK(pending.IsEmpty());
  CHECK_EQ(pending.AddLine("SELECT 2;"), Statements({"SELECT 2;"}));
}

TEST(test_statement_buffer_ignores_semicolons_and_quotes_inside_comments) {
  cli::StatementBuffer trailing;
  CHECK_EQ(trailing.AddLine("SELECT 1; -- it's"), Statements({"SELECT 1;"}));
  CHECK(trailing.IsEmpty());
  CHECK(!trailing.IsInString());

  cli::StatementBuffer only_comment;
  CHECK_EQ(only_comment.AddLine("-- only a comment"), Statements());
  CHECK(only_comment.IsEmpty());

  cli::StatementBuffer inline_comment;
  CHECK_EQ(inline_comment.AddLine("SELECT 1 -- c"), Statements());
  CHECK(!inline_comment.IsEmpty());
  CHECK_EQ(inline_comment.AddLine(";"), Statements({"SELECT 1 -- c\n;"}));

  cli::StatementBuffer in_literal;
  CHECK_EQ(in_literal.AddLine("SELECT '--x';"), Statements({"SELECT '--x';"}));

  cli::StatementBuffer swallowed;
  CHECK_EQ(swallowed.AddLine("SELECT 1--2;"), Statements());
  CHECK(!swallowed.IsEmpty());

  cli::StatementBuffer semicolon_in_comment;
  CHECK_EQ(semicolon_in_comment.AddLine("SELECT 1; -- a; b"),
           Statements({"SELECT 1;"}));
  CHECK(semicolon_in_comment.IsEmpty());
}

TEST(test_statement_buffer_treats_non_ascii_spaces_as_blank) {
  const std::string no_break_space = "\xC2\xA0";
  const std::string ideographic_space = "\xE3\x80\x80";
  const std::string em_space = "\xE2\x80\x83";

  cli::StatementBuffer first;
  CHECK_EQ(first.AddLine("SELECT 1;" + no_break_space),
           Statements({"SELECT 1;"}));
  CHECK(first.IsEmpty());
  cli::StatementBuffer second;
  CHECK_EQ(second.AddLine("SELECT 2;" + ideographic_space),
           Statements({"SELECT 2;"}));
  CHECK(second.IsEmpty());
  cli::StatementBuffer third;
  CHECK_EQ(third.AddLine("SELECT 3;" + em_space), Statements({"SELECT 3;"}));
  CHECK(third.IsEmpty());
  cli::StatementBuffer lone_semicolon;
  CHECK_EQ(lone_semicolon.AddLine(";" + ideographic_space), Statements());
  CHECK(lone_semicolon.IsEmpty());

  // The ends of U+2000 to U+200A are blank; U+200B (zero width space) is not.
  cli::StatementBuffer en_quad;
  CHECK_EQ(en_quad.AddLine("SELECT 4;\xE2\x80\x80"), Statements({"SELECT 4;"}));
  CHECK(en_quad.IsEmpty());
  cli::StatementBuffer hair_space;
  CHECK_EQ(hair_space.AddLine("SELECT 5;\xE2\x80\x8A"),
           Statements({"SELECT 5;"}));
  CHECK(hair_space.IsEmpty());
  cli::StatementBuffer zero_width_space;
  CHECK_EQ(zero_width_space.AddLine("SELECT 6;\xE2\x80\x8B"),
           Statements({"SELECT 6;"}));
  CHECK(!zero_width_space.IsEmpty());
}

TEST(test_statement_buffer_detects_full_width_semicolon_outside_strings) {
  const std::string full_width = "\xEF\xBC\x9B";
  cli::StatementBuffer buffer;

  CHECK(buffer.HasFullWidthSemicolon("SELECT 1" + full_width));
  CHECK(!buffer.HasFullWidthSemicolon("SELECT '" + full_width + "'"));
  CHECK(!buffer.HasFullWidthSemicolon("SELECT 1 -- " + full_width));
  CHECK(!buffer.HasFullWidthSemicolon("SELECT 1;"));

  cli::StatementBuffer in_string;
  in_string.AddLine("SELECT 'a");
  CHECK(!in_string.HasFullWidthSemicolon("b" + full_width));
  CHECK(in_string.HasFullWidthSemicolon("b'" + full_width));
  CHECK(in_string.IsInString());
}
