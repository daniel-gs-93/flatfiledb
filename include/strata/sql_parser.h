#ifndef STRATA_SQL_PARSER_H_
#define STRATA_SQL_PARSER_H_

#include <cstddef>
#include <string>
#include <variant>

#include "strata/domain/ast.h"

namespace strata::adapter {

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

// The number of bytes of the SQL white space character at text[position], or 0
// when there is none: ASCII white space, U+00A0, U+3000 and U+2000 to U+200A
// in UTF-8 (docs/DesignDocument.md section 2.3). Pure.
std::size_t WhitespaceLengthAt(const std::string& text, std::size_t position);

}  // namespace strata::adapter

#endif  // STRATA_SQL_PARSER_H_
