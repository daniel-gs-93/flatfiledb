#ifndef FLATFILEDB_SQL_PARSER_H_
#define FLATFILEDB_SQL_PARSER_H_

#include <cstddef>
#include <string>
#include <variant>

#include "flatfiledb/domain/ast.h"

namespace flatfiledb::adapter {

struct ParseError {
  // 1-based position of the offending token's first character; for an error at
  // the end of the input it is the input length plus 1; for an unterminated
  // string literal it is the position of the opening quote.
  std::size_t column;
  std::string description;
};

using ParseResult = std::variant<domain::Statement, ParseError>;

// Parses one SQL statement (SELECT, INSERT, UPDATE or DELETE, with an optional
// trailing semicolon) following docs/DesignDocument.md section 2.3. Text
// that starts with '.' is not SQL and yields a ParseError. Pure.
ParseResult ParseStatement(const std::string& text);

}  // namespace flatfiledb::adapter

#endif  // FLATFILEDB_SQL_PARSER_H_
