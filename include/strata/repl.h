#ifndef STRATA_REPL_H_
#define STRATA_REPL_H_

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "strata/domain/value.h"
#include "strata/sql_parser.h"

namespace strata::cli {

// A database to open before the first line is read, as if the user had typed
// `.open <directory> [--parallel]` (the command line's path).
struct InitialOpen {
  std::string directory;
  bool is_parallel = false;
};

// Reads lines from input until it ends, runs the commands and statements they
// hold (docs/DesignDocument.md section 3.6), and writes the results to output.
// When is_interactive is set, a prompt is written before every read:
// "strata> " normally, and a continuation prompt of the same width while a
// statement is in progress or inside a string literal; `.clear` then writes the
// clear-screen control codes. initial_open, when set, is opened first and its
// result printed like `.open`'s. The only function that touches the streams.
void RunRepl(std::istream& input, std::ostream& output, bool is_interactive,
             const std::optional<InitialOpen>& initial_open = std::nullopt);

// The `.schema` text of one table: a CREATE TABLE statement with one column per
// line, ending in ';'. catalog holds every table, so that a REFERENCES can name
// the column of its target (ADR-0018).
std::string FormatCreateTable(const domain::TableSchema& schema,
                              const std::vector<domain::TableSchema>& catalog);

// "syntax error at column <n>: <description>", or for an error on the second or
// a later line of a multi-line statement "syntax error at line <l>, column <c>:
// <description>".
std::string FormatParseError(const std::string& statement,
                             const adapter::ParseError& error);

}  // namespace strata::cli

#endif  // STRATA_REPL_H_
