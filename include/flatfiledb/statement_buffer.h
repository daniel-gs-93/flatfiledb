#ifndef FLATFILEDB_STATEMENT_BUFFER_H_
#define FLATFILEDB_STATEMENT_BUFFER_H_

#include <string>
#include <vector>

namespace flatfiledb::cli {

// Turns the lines a user types into statements (docs/DesignDocument.md section
// 3.6, ADR-0017). A statement ends at a ';' outside a string literal and may
// span several lines, which are joined with '\n'. A single quote inside a
// literal is written twice. Pure: no stream access.
class StatementBuffer {
 public:
  // Adds one line, without its line ending. Returns the statements the line
  // completed, in order, each including its ';'. A statement that continues
  // after the line stays buffered. Statements made of only ';' are dropped.
  // Leading white space of a statement is kept, so that error columns count
  // from where the user began typing.
  std::vector<std::string> AddLine(const std::string& line);

  // True when no statement is in progress: nothing is buffered but white
  // space.
  bool IsEmpty() const { return pending_.empty(); }

 private:
  std::string pending_;
  bool is_in_string_ = false;
};

}  // namespace flatfiledb::cli

#endif  // FLATFILEDB_STATEMENT_BUFFER_H_
