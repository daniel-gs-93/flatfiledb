#ifndef FLATFILEDB_REPL_H_
#define FLATFILEDB_REPL_H_

#include <iosfwd>
#include <string>
#include <vector>

#include "flatfiledb/domain/value.h"
#include "flatfiledb/sql_parser.h"

namespace flatfiledb::cli {

// Reads lines from input until it ends, runs the commands and statements they
// hold (docs/DesignDocument.md section 3.6), and writes the results to output.
// When is_interactive is set, a prompt is written before every read:
// "flatfiledb> " normally and a continuation prompt of the same width while a
// statement is in progress. The only function that touches the streams.
void RunRepl(std::istream& input, std::ostream& output, bool is_interactive);

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

}  // namespace flatfiledb::cli

#endif  // FLATFILEDB_REPL_H_
