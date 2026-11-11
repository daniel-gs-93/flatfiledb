#ifndef FLATFILEDB_STATEMENT_BUFFER_H_
#define FLATFILEDB_STATEMENT_BUFFER_H_

#include <string>
#include <vector>

namespace flatfiledb::cli {

// Turns the lines a user types into statements (docs/DesignDocument.md section
// 3.6, ADR-0017, ADR-0023). A statement ends at a ';' outside a string literal
// and outside a `--` comment, and may span several lines, which are joined with
// '\n'. A single quote inside a literal is written twice. Pure: no stream
// access.
class StatementBuffer {
 public:
  // Adds one line, without its line ending. Returns the statements the line
  // completed, in order, each including its ';'. A statement that continues
  // after the line stays buffered. Statements made of only ';' are dropped, and
  // so are text that is only white space or a comment. Leading white space of a
  // statement is kept, so that error columns count from where the user began
  // typing.
  std::vector<std::string> AddLine(const std::string& line);

  // True when no statement is in progress: nothing is buffered but white space
  // and comments.
  bool IsEmpty() const { return pending_.empty(); }

  // True when the buffered text ends inside a string literal.
  bool IsInString() const { return is_in_string_; }

  // Discards the buffered text.
  void Cancel();

  // True when `line`, read from the current state, has a full-width semicolon
  // (U+FF1B) outside a string literal and outside a comment. Does not change
  // the buffer.
  bool HasFullWidthSemicolon(const std::string& line) const;

 private:
  std::string pending_;
  bool is_in_string_ = false;
};

// True when text has nothing but SQL white space (docs/DesignDocument.md
// section 2.3).
bool IsSqlBlank(const std::string& text);

// The text without leading and trailing SQL white space.
std::string TrimSqlWhitespace(const std::string& text);

}  // namespace flatfiledb::cli

#endif  // FLATFILEDB_STATEMENT_BUFFER_H_
