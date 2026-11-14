#include "flatfiledb/sql_parser.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/value.h"

// A tokenizer and a recursive-descent parser for the SQL subset of
// docs/DesignDocument.md section 2.3. Both are pure: they read the input text
// and build the result, with no IO and no state outside the objects local to
// one call.
//
// Expression grammar, lowest precedence first. The block is excluded from
// formatting to keep one rule per line.
// clang-format off
//   or_expr    := and_expr { OR and_expr }
//   and_expr   := comparison { AND comparison }
//   comparison := additive [ cmp_op additive ]
//   cmp_op     := "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" | LIKE
//   additive   := multiplicative { ("+" | "-") multiplicative }
//   multiplicative := concat { ("*" | "/") concat }
//   concat     := unary { "||" unary }
//   unary      := "-" unary | primary
//   primary    := number | string | "(" or_expr ")" | aggregate | call | column
//   aggregate  := COUNT "(" ( "*" | or_expr ) ")" | SUM "(" or_expr ")"
//   call       := name "(" [ or_expr { "," or_expr } ] ")"
//               | CAST "(" or_expr AS type ")"
//   column     := identifier [ "." identifier ]
// A call is an identifier directly followed by "(": name is AVG, MIN, MAX (an
// aggregate) or one of the scalar functions, and is not a reserved word.
// clang-format on

namespace flatfiledb::adapter {

namespace {

// Parser recursion and AST depth are bounded so that hostile input yields a
// ParseError, and so that destroying or walking the AST cannot exhaust the
// stack.
constexpr std::size_t kMaxNestingDepth = 100;
constexpr std::size_t kMaxExpressionDepth = 500;

enum class TokenKind {
  kEnd,
  kInvalid,  // a lexical error; Token::text holds the description
  kIdentifier,
  kNumber,
  kString,
  kLParen,
  kRParen,
  kComma,
  kDot,
  kStar,
  kSlash,
  kConcat,
  kPlus,
  kMinus,
  kSemicolon,
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kSelect,
  kFrom,
  kWhere,
  kJoin,
  kOn,
  kAs,
  kGroup,
  kBy,
  kOrder,
  kAsc,
  kDesc,
  kInsert,
  kInto,
  kValues,
  kUpdate,
  kSet,
  kDelete,
  kAnd,
  kOr,
  kLike,
  kLimit,
  kOffset,
  kCount,
  kSum,
  kNull,
};

struct Token {
  TokenKind kind;
  std::size_t offset;  // 0-based index of the first character in the input
  std::size_t length;  // number of input characters the token spans
  // Identifier spelling, digits of a number, decoded content of a string, or
  // the description of a lexical error; empty for punctuation and keywords.
  std::string text;
};

struct Keyword {
  const char* word;  // lower case
  TokenKind kind;
};

// The table has a fixed size, so the linear scan per identifier is bounded.
constexpr Keyword kKeywords[] = {
    {"select", TokenKind::kSelect}, {"from", TokenKind::kFrom},
    {"where", TokenKind::kWhere},   {"join", TokenKind::kJoin},
    {"on", TokenKind::kOn},         {"as", TokenKind::kAs},
    {"group", TokenKind::kGroup},   {"by", TokenKind::kBy},
    {"order", TokenKind::kOrder},   {"asc", TokenKind::kAsc},
    {"desc", TokenKind::kDesc},     {"insert", TokenKind::kInsert},
    {"into", TokenKind::kInto},     {"values", TokenKind::kValues},
    {"update", TokenKind::kUpdate}, {"set", TokenKind::kSet},
    {"delete", TokenKind::kDelete}, {"and", TokenKind::kAnd},
    {"or", TokenKind::kOr},         {"like", TokenKind::kLike},
    {"limit", TokenKind::kLimit},   {"offset", TokenKind::kOffset},
    {"count", TokenKind::kCount},   {"sum", TokenKind::kSum},
    {"null", TokenKind::kNull},
};

// Words outside the supported grammar that are not reserved (so they lex as
// identifiers). They get a dedicated error message, and the join-type words
// are refused as a table alias before JOIN, where accepting them would turn
// `LEFT JOIN` into an inner join.
constexpr const char* kUnsupportedWords[] = {
    "not",   "in",   "is",    "between", "having", "distinct", "union",
    "inner", "left", "right", "full",    "outer",  "cross",    "natural",
};

bool IsDigit(char letter) { return letter >= '0' && letter <= '9'; }

bool IsIdentifierStart(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
         letter == '_';
}

bool IsIdentifierPart(char letter) {
  return IsIdentifierStart(letter) || IsDigit(letter);
}

bool IsSpace(char letter) {
  return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r' ||
         letter == '\v' || letter == '\f';
}

std::string ToUpperAscii(const std::string& text) {
  std::string upper = text;
  for (char& letter : upper) {
    if (letter >= 'a' && letter <= 'z') {
      letter = static_cast<char>(letter - 'a' + 'A');
    }
  }
  return upper;
}

bool IsUnsupportedWord(const Token& token) {
  if (token.kind != TokenKind::kIdentifier) {
    return false;
  }
  const std::string lowered = domain::ToLower(token.text);
  for (const char* word : kUnsupportedWords) {
    if (lowered == word) {
      return true;
    }
  }
  return false;
}

Token MakeToken(TokenKind kind, std::size_t offset, std::size_t length,
                std::string text = "") {
  return Token{kind, offset, length, std::move(text)};
}

TokenKind ClassifyWord(const std::string& word) {
  const std::string lowered = domain::ToLower(word);
  for (const Keyword& keyword : kKeywords) {
    if (lowered == keyword.word) {
      return keyword.kind;
    }
  }
  return TokenKind::kIdentifier;
}

Token LexWord(const std::string& input, std::size_t start) {
  std::size_t end = start;
  while (end < input.size() && IsIdentifierPart(input[end])) {
    ++end;
  }
  std::string word = input.substr(start, end - start);
  const TokenKind kind = ClassifyWord(word);
  return MakeToken(kind, start, end - start, std::move(word));
}

// digits or digits.digits; the fraction needs a digit after the '.', so `1.`
// lexes as the number `1` followed by a '.'.
Token LexNumber(const std::string& input, std::size_t start) {
  std::size_t end = start;
  while (end < input.size() && IsDigit(input[end])) {
    ++end;
  }
  const bool has_fraction =
      end + 1 < input.size() && input[end] == '.' && IsDigit(input[end + 1]);
  if (has_fraction) {
    ++end;
    while (end < input.size() && IsDigit(input[end])) {
      ++end;
    }
  }
  return MakeToken(TokenKind::kNumber, start, end - start,
                   input.substr(start, end - start));
}

// A doubled quote inside the literal stands for one quote character.
Token LexString(const std::string& input, std::size_t start) {
  std::string content;
  std::size_t position = start + 1;
  while (position < input.size()) {
    if (input[position] != '\'') {
      content.push_back(input[position]);
      ++position;
      continue;
    }
    const bool is_escaped_quote =
        position + 1 < input.size() && input[position + 1] == '\'';
    if (!is_escaped_quote) {
      return MakeToken(TokenKind::kString, start, position + 1 - start,
                       std::move(content));
    }
    content.push_back('\'');
    position += 2;
  }
  return MakeToken(TokenKind::kInvalid, start, input.size() - start,
                   "unterminated string literal");
}

// The description of the unexpected character at input[start] and the number
// of bytes it spans: a visible ASCII character in quotes, a valid UTF-8
// character by its code point, anything else by its byte value.
struct UnexpectedCharacter {
  std::string description;
  std::size_t length;
};

UnexpectedCharacter DescribeUnexpectedCharacter(const std::string& input,
                                                std::size_t start) {
  const unsigned char lead = static_cast<unsigned char>(input[start]);
  if (lead >= 0x20 && lead <= 0x7e) {
    return {std::string("unexpected character '") + input[start] + "'", 1};
  }
  std::size_t length = 1;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
  }
  bool is_valid = length > 1 && start + length <= input.size();
  uint32_t code_point = lead;
  if (length == 2) {
    code_point = lead & 0x1F;
  }
  if (length == 3) {
    code_point = lead & 0x0F;
  }
  if (length == 4) {
    code_point = lead & 0x07;
  }
  for (std::size_t i = 1; is_valid && i < length; ++i) {
    const unsigned char follower = static_cast<unsigned char>(input[start + i]);
    if (follower < 0x80 || follower > 0xBF) {
      is_valid = false;
    } else {
      code_point = (code_point << 6) | (follower & 0x3F);
    }
  }
  if (is_valid) {
    std::ostringstream text;
    text << "unexpected character U+" << std::uppercase << std::hex
         << std::setfill('0') << std::setw(4) << code_point;
    return {text.str(), length};
  }
  return {"unexpected character with byte value " +
              std::to_string(static_cast<unsigned>(lead)),
          1};
}

Token LexSymbol(const std::string& input, std::size_t start) {
  const char current = input[start];
  const char next = start + 1 < input.size() ? input[start + 1] : '\0';
  switch (current) {
    case '(':
      return MakeToken(TokenKind::kLParen, start, 1);
    case ')':
      return MakeToken(TokenKind::kRParen, start, 1);
    case ',':
      return MakeToken(TokenKind::kComma, start, 1);
    case '.':
      return MakeToken(TokenKind::kDot, start, 1);
    case '*':
      return MakeToken(TokenKind::kStar, start, 1);
    case '/':
      return MakeToken(TokenKind::kSlash, start, 1);
    case '|':
      if (next == '|') {
        return MakeToken(TokenKind::kConcat, start, 2);
      }
      break;
    case '+':
      return MakeToken(TokenKind::kPlus, start, 1);
    case '-':
      return MakeToken(TokenKind::kMinus, start, 1);
    case ';':
      return MakeToken(TokenKind::kSemicolon, start, 1);
    case '=':
      return MakeToken(TokenKind::kEq, start, 1);
    case '<':
      if (next == '>') {
        return MakeToken(TokenKind::kNe, start, 2);
      }
      if (next == '=') {
        return MakeToken(TokenKind::kLe, start, 2);
      }
      return MakeToken(TokenKind::kLt, start, 1);
    case '>':
      if (next == '=') {
        return MakeToken(TokenKind::kGe, start, 2);
      }
      return MakeToken(TokenKind::kGt, start, 1);
    case '!':
      if (next == '=') {
        return MakeToken(TokenKind::kNe, start, 2);
      }
      break;
    default:
      break;
  }
  UnexpectedCharacter unexpected = DescribeUnexpectedCharacter(input, start);
  return MakeToken(TokenKind::kInvalid, start, unexpected.length,
                   std::move(unexpected.description));
}

Token LexToken(const std::string& input, std::size_t start) {
  const char first_char = input[start];
  if (IsIdentifierStart(first_char)) {
    return LexWord(input, start);
  }
  if (IsDigit(first_char)) {
    return LexNumber(input, start);
  }
  if (first_char == '\'') {
    return LexString(input, start);
  }
  return LexSymbol(input, start);
}

// The position of the first character at or after `position` that is neither
// white space nor part of a `--` comment (which runs to the end of its line).
std::size_t SkipBlanksAndComments(const std::string& input,
                                  std::size_t position) {
  while (position < input.size()) {
    const std::size_t blank = WhitespaceLengthAt(input, position);
    if (blank > 0) {
      position += blank;
      continue;
    }
    if (std::string_view(input).substr(position).starts_with("--")) {
      while (position < input.size() && input[position] != '\n') {
        ++position;
      }
      continue;
    }
    break;
  }
  return position;
}

// Always ends with a kEnd token. A lexical error becomes a kInvalid token that
// stops the scan, so a syntax error located before it is still reported first.
std::vector<Token> Tokenize(const std::string& input) {
  std::vector<Token> tokens;
  std::size_t position = 0;
  while (true) {
    position = SkipBlanksAndComments(input, position);
    if (position >= input.size()) {
      break;
    }
    Token token = LexToken(input, position);
    const bool is_invalid = token.kind == TokenKind::kInvalid;
    position += token.length;
    tokens.push_back(std::move(token));
    if (is_invalid) {
      break;
    }
  }
  tokens.push_back(MakeToken(TokenKind::kEnd, input.size(), 0));
  return tokens;
}

// `digits` holds only digits. The magnitude is accumulated unsigned so that
// the int64_t minimum is representable; the overflow test is
// magnitude * 10 + digit <= limit, rearranged to avoid overflowing itself.
std::optional<domain::Value> ConvertIntLiteral(const std::string& digits,
                                               bool is_negative) {
  const uint64_t limit =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) +
      (is_negative ? 1 : 0);
  uint64_t magnitude = 0;
  for (const char letter : digits) {
    const uint64_t digit = static_cast<uint64_t>(letter - '0');
    if (magnitude > (limit - digit) / 10) {
      return std::nullopt;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (!is_negative) {
    return domain::Value(static_cast<int64_t>(magnitude));
  }
  if (magnitude == limit) {
    return domain::Value(std::numeric_limits<int64_t>::min());
  }
  return domain::Value(-static_cast<int64_t>(magnitude));
}

// A stream imbued with the classic locale keeps the result independent of the
// process locale. Overflow to infinity and underflow set the fail bit.
std::optional<domain::Value> ConvertDoubleLiteral(const std::string& text,
                                                  bool is_negative) {
  std::istringstream stream(text);
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> value;
  if (stream.fail() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return domain::Value(is_negative ? -value : value);
}

std::optional<domain::Value> ConvertNumberLiteral(const std::string& text,
                                                  bool is_negative) {
  const bool is_double = text.find('.') != std::string::npos;
  return is_double ? ConvertDoubleLiteral(text, is_negative)
                   : ConvertIntLiteral(text, is_negative);
}

domain::ExprPtr MakeLiteral(domain::Value value) {
  auto expr = std::make_unique<domain::Expr>();
  expr->node = domain::Literal{std::move(value)};
  return expr;
}

domain::ExprPtr MakeColumn(std::string qualifier, std::string name) {
  auto expr = std::make_unique<domain::Expr>();
  expr->node = domain::ColumnRef{std::move(qualifier), std::move(name)};
  return expr;
}

domain::ExprPtr MakeBinary(domain::BinaryOp op, domain::ExprPtr lhs,
                           domain::ExprPtr rhs) {
  auto expr = std::make_unique<domain::Expr>();
  expr->node = domain::BinaryExpr{op, std::move(lhs), std::move(rhs)};
  return expr;
}

domain::ExprPtr MakeAggregate(domain::AggregateFn fn, domain::ExprPtr arg) {
  auto expr = std::make_unique<domain::Expr>();
  expr->node = domain::AggregateCall{fn, std::move(arg)};
  return expr;
}

domain::ExprPtr MakeFunction(domain::ScalarFn fn,
                             std::vector<domain::ExprPtr> args,
                             domain::DataType cast_type) {
  auto expr = std::make_unique<domain::Expr>();
  expr->node = domain::FunctionCall{fn, std::move(args), cast_type};
  return expr;
}

// Recursion is bounded because every child was checked against
// kMaxExpressionDepth when it was built.
std::size_t ExprDepth(const domain::Expr& expr) {
  if (const auto* binary = std::get_if<domain::BinaryExpr>(&expr.node)) {
    return 1 + std::max(ExprDepth(*binary->lhs), ExprDepth(*binary->rhs));
  }
  if (const auto* aggregate = std::get_if<domain::AggregateCall>(&expr.node)) {
    return 1 + (aggregate->arg ? ExprDepth(*aggregate->arg) : 0);
  }
  if (const auto* function = std::get_if<domain::FunctionCall>(&expr.node)) {
    std::size_t deepest_arg = 0;
    for (const domain::ExprPtr& arg : function->args) {
      deepest_arg = std::max(deepest_arg, ExprDepth(*arg));
    }
    return 1 + deepest_arg;
  }
  return 1;
}

std::optional<domain::BinaryOp> ComparisonOperator(TokenKind kind) {
  switch (kind) {
    case TokenKind::kEq:
      return domain::BinaryOp::kEq;
    case TokenKind::kNe:
      return domain::BinaryOp::kNe;
    case TokenKind::kLt:
      return domain::BinaryOp::kLt;
    case TokenKind::kLe:
      return domain::BinaryOp::kLe;
    case TokenKind::kGt:
      return domain::BinaryOp::kGt;
    case TokenKind::kGe:
      return domain::BinaryOp::kGe;
    case TokenKind::kLike:
      return domain::BinaryOp::kLike;
    default:
      return std::nullopt;
  }
}

// The scalar function that `word` (upper case) names, or std::nullopt.
std::optional<domain::ScalarFn> ScalarFunctionNamed(const std::string& word) {
  struct Entry {
    const char* name;
    domain::ScalarFn fn;
  };
  constexpr Entry kFunctions[] = {
      {"LOWER", domain::ScalarFn::kLower},
      {"UPPER", domain::ScalarFn::kUpper},
      {"LENGTH", domain::ScalarFn::kLength},
      {"SUBSTR", domain::ScalarFn::kSubstr},
      {"TRIM", domain::ScalarFn::kTrim},
      {"ABS", domain::ScalarFn::kAbs},
      {"ROUND", domain::ScalarFn::kRound},
      {"CAST", domain::ScalarFn::kCast},
  };
  for (const Entry& entry : kFunctions) {
    if (word == entry.name) {
      return entry.fn;
    }
  }
  return std::nullopt;
}

// The aggregate that `word` (upper case) names among the functions that are
// not reserved words, or std::nullopt.
std::optional<domain::AggregateFn> AggregateNamed(const std::string& word) {
  if (word == "AVG") {
    return domain::AggregateFn::kAvg;
  }
  if (word == "MIN") {
    return domain::AggregateFn::kMin;
  }
  if (word == "MAX") {
    return domain::AggregateFn::kMax;
  }
  return std::nullopt;
}

bool StartsOperand(TokenKind kind) {
  switch (kind) {
    case TokenKind::kIdentifier:
    case TokenKind::kNumber:
    case TokenKind::kString:
    case TokenKind::kLParen:
    case TokenKind::kMinus:
    case TokenKind::kCount:
    case TokenKind::kSum:
      return true;
    default:
      return false;
  }
}

// Counts one level of recursion for the lifetime of the object.
class NestingScope {
 public:
  explicit NestingScope(std::size_t* depth) : depth_(depth) { ++*depth_; }
  ~NestingScope() { --*depth_; }
  NestingScope(const NestingScope&) = delete;
  NestingScope& operator=(const NestingScope&) = delete;

 private:
  std::size_t* depth_;
};

// Failure convention: a parse function that fails records the first error in
// error_ and returns null or std::nullopt; its caller returns immediately.
class Parser {
 public:
  Parser(const std::string& input, std::vector<Token> tokens)
      : input_(input), tokens_(std::move(tokens)) {}

  ParseResult Run() {
    std::optional<domain::Statement> statement = ParseStatementBody();
    if (statement.has_value() && ParseStatementEnd()) {
      return ParseResult(std::move(*statement));
    }
    assert(error_.has_value());
    return *error_;
  }

 private:
  const Token& Current() const { return tokens_[position_]; }

  // The last token (kEnd) stands in for every position past the end.
  const Token& Peek(std::size_t distance) const {
    return tokens_[std::min(position_ + distance, tokens_.size() - 1)];
  }

  void Advance() {
    const Token& token = tokens_[position_];
    if (token.kind == TokenKind::kEnd) {
      return;
    }
    previous_end_ = token.offset + token.length;
    ++position_;
  }

  bool Match(TokenKind kind) {
    if (Current().kind != kind) {
      return false;
    }
    Advance();
    return true;
  }

  // Keeps the first error: later failures are consequences of it.
  void FailAtOffset(std::size_t offset, const std::string& description) {
    if (error_.has_value()) {
      return;
    }
    error_ = ParseError{offset + 1, description};
  }

  void Fail(const Token& token, const std::string& description) {
    FailAtOffset(token.offset, description);
  }

  std::string DescribeToken(const Token& token) const {
    if (token.kind == TokenKind::kEnd) {
      return "end of input";
    }
    return "'" + input_.substr(token.offset, token.length) + "'";
  }

  // Reports the current token as unexpected. A lexical error, NULL and the
  // known unsupported words get their own description.
  void FailExpected(const std::string& expected) {
    const Token& token = Current();
    if (token.kind == TokenKind::kInvalid) {
      Fail(token, token.text);
    } else if (token.kind == TokenKind::kNull) {
      Fail(token, "NULL is not supported");
    } else if (IsUnsupportedWord(token)) {
      Fail(token, ToUpperAscii(token.text) + " is not supported");
    } else {
      Fail(token, "expected " + expected + ", found " + DescribeToken(token));
    }
  }

  bool Expect(TokenKind kind, const std::string& expected) {
    if (Match(kind)) {
      return true;
    }
    FailExpected(expected);
    return false;
  }

  bool ExpectIdentifier(const std::string& expected, std::string* name) {
    if (Current().kind != TokenKind::kIdentifier) {
      FailExpected(expected);
      return false;
    }
    *name = Current().text;
    Advance();
    return true;
  }

  // Statements.

  template <typename StatementType>
  static std::optional<domain::Statement> ToStatement(
      std::optional<StatementType> statement) {
    if (!statement.has_value()) {
      return std::nullopt;
    }
    return domain::Statement(std::move(*statement));
  }

  std::optional<domain::Statement> ParseStatementBody() {
    switch (Current().kind) {
      case TokenKind::kSelect:
        return ToStatement(ParseSelect());
      case TokenKind::kInsert:
        return ToStatement(ParseInsert());
      case TokenKind::kUpdate:
        return ToStatement(ParseUpdate());
      case TokenKind::kDelete:
        return ToStatement(ParseDelete());
      default:
        break;
    }
    if (IsWord(Current(), "CREATE")) {
      return ToStatement(ParseCreateTable());
    }
    if (IsWord(Current(), "ALTER")) {
      return ToStatement(ParseAlterTable());
    }
    if (IsWord(Current(), "DROP")) {
      return ToStatement(ParseDropTable());
    }
    // Reported at column 1 whatever the token is, so leading blanks and text
    // such as ".open" share one description.
    error_ = ParseError{1, "unrecognized statement"};
    return std::nullopt;
  }

  bool ParseStatementEnd() {
    if (Match(TokenKind::kSemicolon)) {
      if (Current().kind == TokenKind::kEnd) {
        return true;
      }
      FailExpected("end of input after ';'");
      return false;
    }
    if (Current().kind == TokenKind::kEnd) {
      return true;
    }
    FailExpected("';' or end of input");
    return false;
  }

  std::optional<domain::SelectStatement> ParseSelect() {
    Advance();
    domain::SelectStatement select;
    if (!ParseSelectItems(&select.items)) {
      return std::nullopt;
    }
    if (!Expect(TokenKind::kFrom, "FROM")) {
      return std::nullopt;
    }
    std::optional<domain::TableRef> from = ParseTableRef();
    if (!from.has_value()) {
      return std::nullopt;
    }
    select.from = std::move(*from);
    while (Match(TokenKind::kJoin)) {
      std::optional<domain::TableRef> table = ParseTableRef();
      if (!table.has_value()) {
        return std::nullopt;
      }
      if (!Expect(TokenKind::kOn, "ON")) {
        return std::nullopt;
      }
      domain::ExprPtr condition = ParseExpression();
      if (condition == nullptr) {
        return std::nullopt;
      }
      select.joins.push_back(
          domain::JoinClause{std::move(*table), std::move(condition)});
    }
    if (!ParseOptionalWhere(&select.where)) {
      return std::nullopt;
    }
    if (Match(TokenKind::kGroup)) {
      if (!Expect(TokenKind::kBy, "BY")) {
        return std::nullopt;
      }
      if (!ParseExpressionList(&select.group_by)) {
        return std::nullopt;
      }
    }
    if (Match(TokenKind::kOrder)) {
      if (!Expect(TokenKind::kBy, "BY")) {
        return std::nullopt;
      }
      if (!ParseOrderItems(&select.order_by)) {
        return std::nullopt;
      }
    }
    if (!ParseLimitClause(&select)) {
      return std::nullopt;
    }
    return select;
  }

  // [LIMIT <n> [OFFSET <n>]]; OFFSET alone is an error.
  bool ParseLimitClause(domain::SelectStatement* select) {
    if (Current().kind == TokenKind::kOffset) {
      Fail(Current(), "OFFSET requires LIMIT");
      return false;
    }
    if (!Match(TokenKind::kLimit)) {
      return true;
    }
    int64_t limit = 0;
    if (!ParseNonNegativeInteger("LIMIT", &limit)) {
      return false;
    }
    select->limit = limit;
    if (!Match(TokenKind::kOffset)) {
      return true;
    }
    return ParseNonNegativeInteger("OFFSET", &select->offset);
  }

  // The current token must be an integer literal without sign or fraction.
  bool ParseNonNegativeInteger(const char* clause, int64_t* value) {
    const Token& token = Current();
    const bool is_integer = token.kind == TokenKind::kNumber &&
                            token.text.find('.') == std::string::npos;
    if (!is_integer) {
      Fail(token,
           std::string(clause) + " requires a non-negative integer literal");
      return false;
    }
    std::optional<domain::Value> converted =
        ConvertIntLiteral(token.text, /*is_negative=*/false);
    if (!converted.has_value()) {
      Fail(token, "numeric literal out of range");
      return false;
    }
    *value = std::get<int64_t>(*converted);
    Advance();
    return true;
  }

  std::optional<domain::InsertStatement> ParseInsert() {
    Advance();
    domain::InsertStatement insert;
    if (!Expect(TokenKind::kInto, "INTO")) {
      return std::nullopt;
    }
    if (!ExpectIdentifier("table name", &insert.table)) {
      return std::nullopt;
    }
    if (Match(TokenKind::kLParen)) {
      do {
        std::string column;
        if (!ExpectIdentifier("column name", &column)) {
          return std::nullopt;
        }
        insert.columns.push_back(std::move(column));
      } while (Match(TokenKind::kComma));
      if (!Expect(TokenKind::kRParen, "',' or ')'")) {
        return std::nullopt;
      }
    }
    if (!Expect(TokenKind::kValues, "VALUES")) {
      return std::nullopt;
    }
    do {
      if (!Expect(TokenKind::kLParen, "'('")) {
        return std::nullopt;
      }
      std::vector<domain::ExprPtr> row;
      if (!ParseExpressionList(&row)) {
        return std::nullopt;
      }
      if (!Expect(TokenKind::kRParen, "',' or ')'")) {
        return std::nullopt;
      }
      insert.rows.push_back(std::move(row));
    } while (Match(TokenKind::kComma));
    return insert;
  }

  std::optional<domain::UpdateStatement> ParseUpdate() {
    Advance();
    domain::UpdateStatement update;
    if (!ExpectIdentifier("table name", &update.table)) {
      return std::nullopt;
    }
    if (!Expect(TokenKind::kSet, "SET")) {
      return std::nullopt;
    }
    do {
      domain::Assignment assignment;
      if (!ExpectIdentifier("column name", &assignment.column)) {
        return std::nullopt;
      }
      if (!Expect(TokenKind::kEq, "'='")) {
        return std::nullopt;
      }
      assignment.value = ParseExpression();
      if (assignment.value == nullptr) {
        return std::nullopt;
      }
      update.assignments.push_back(std::move(assignment));
    } while (Match(TokenKind::kComma));
    if (!ParseOptionalWhere(&update.where)) {
      return std::nullopt;
    }
    return update;
  }

  std::optional<domain::DeleteStatement> ParseDelete() {
    Advance();
    domain::DeleteStatement delete_statement;
    if (!Expect(TokenKind::kFrom, "FROM")) {
      return std::nullopt;
    }
    if (!ExpectIdentifier("table name", &delete_statement.table)) {
      return std::nullopt;
    }
    if (!ParseOptionalWhere(&delete_statement.where)) {
      return std::nullopt;
    }
    return delete_statement;
  }

  // Clauses.

  // DDL. Its words are identifiers that count as keywords only where the
  // grammar expects them, so they stay usable as table and column names.

  bool IsWord(const Token& token, const char* upper_word) const {
    return token.kind == TokenKind::kIdentifier &&
           ToUpperAscii(token.text) == upper_word;
  }

  bool MatchWord(const char* upper_word) {
    if (!IsWord(Current(), upper_word)) {
      return false;
    }
    Advance();
    return true;
  }

  bool ExpectWord(const char* upper_word) {
    if (MatchWord(upper_word)) {
      return true;
    }
    FailExpected(std::string("'") + upper_word + "'");
    return false;
  }

  std::optional<domain::CreateTableStatement> ParseCreateTable() {
    Advance();  // CREATE
    if (!ExpectWord("TABLE")) {
      return std::nullopt;
    }
    domain::CreateTableStatement statement;
    if (!ExpectIdentifier("a table name", &statement.table)) {
      return std::nullopt;
    }
    if (!Expect(TokenKind::kLParen, "'('")) {
      return std::nullopt;
    }
    do {
      std::optional<domain::ColumnDefinition> column = ParseColumnDefinition();
      if (!column.has_value()) {
        return std::nullopt;
      }
      statement.columns.push_back(std::move(*column));
    } while (Match(TokenKind::kComma));
    if (!Expect(TokenKind::kRParen, "',' or ')'")) {
      return std::nullopt;
    }
    return statement;
  }

  std::optional<domain::AddColumnStatement> ParseAlterTable() {
    Advance();  // ALTER
    if (!ExpectWord("TABLE")) {
      return std::nullopt;
    }
    domain::AddColumnStatement statement;
    if (!ExpectIdentifier("a table name", &statement.table)) {
      return std::nullopt;
    }
    if (!ExpectWord("ADD")) {
      return std::nullopt;
    }
    MatchWord("COLUMN");
    std::optional<domain::ColumnDefinition> column = ParseColumnDefinition();
    if (!column.has_value()) {
      return std::nullopt;
    }
    statement.column = std::move(*column);
    return statement;
  }

  std::optional<domain::DropTableStatement> ParseDropTable() {
    Advance();  // DROP
    if (!ExpectWord("TABLE")) {
      return std::nullopt;
    }
    domain::DropTableStatement statement;
    if (!ExpectIdentifier("a table name", &statement.table)) {
      return std::nullopt;
    }
    return statement;
  }

  // The length of VARCHAR(n) and CHAR(n) is read and not kept.
  bool ParseTypeLength() {
    if (!Expect(TokenKind::kLParen, "'('")) {
      return false;
    }
    const Token& length = Current();
    bool is_positive_integer =
        length.kind == TokenKind::kNumber &&
        length.text.find_first_not_of("0123456789") == std::string::npos &&
        length.text.find_first_not_of('0') != std::string::npos;
    if (!is_positive_integer) {
      FailExpected("a positive length");
      return false;
    }
    Advance();
    return Expect(TokenKind::kRParen, "')'");
  }

  bool ParseColumnType(domain::DataType* type) {
    const Token& token = Current();
    if (token.kind == TokenKind::kIdentifier) {
      const std::string word = ToUpperAscii(token.text);
      if (word == "INT" || word == "INTEGER" || word == "BIGINT") {
        Advance();
        *type = domain::DataType::kInt;
        return true;
      }
      if (word == "DOUBLE" || word == "REAL" || word == "FLOAT") {
        Advance();
        *type = domain::DataType::kDouble;
        return true;
      }
      if (word == "TEXT") {
        Advance();
        *type = domain::DataType::kText;
        return true;
      }
      if (word == "VARCHAR" || word == "CHAR") {
        Advance();
        *type = domain::DataType::kText;
        return ParseTypeLength();
      }
    }
    FailExpected("a column type");
    return false;
  }

  // A constraint may appear once per column, in any order. AUTOINCREMENT is
  // part of PRIMARY KEY and is read there.
  std::optional<domain::ColumnDefinition> ParseColumnDefinition() {
    domain::ColumnDefinition column;
    if (!ExpectIdentifier("a column name", &column.name)) {
      return std::nullopt;
    }
    if (!ParseColumnType(&column.type)) {
      return std::nullopt;
    }

    bool has_primary_key = false;
    bool has_unique = false;
    bool has_not_null = false;
    bool has_default = false;
    bool has_references = false;
    while (true) {
      const Token& token = Current();
      if (IsWord(token, "PRIMARY")) {
        if (has_primary_key) {
          return FailDuplicate(token, "PRIMARY KEY");
        }
        Advance();
        if (!ExpectWord("KEY")) {
          return std::nullopt;
        }
        has_primary_key = true;
        column.is_primary_key = true;
        column.is_auto_increment = MatchWord("AUTOINCREMENT");
      } else if (IsWord(token, "UNIQUE")) {
        if (has_unique) {
          return FailDuplicate(token, "UNIQUE");
        }
        Advance();
        has_unique = true;
        column.is_unique = true;
      } else if (IsWord(token, "NOT")) {
        if (has_not_null) {
          return FailDuplicate(token, "NOT NULL");
        }
        Advance();
        if (!Expect(TokenKind::kNull, "NULL")) {
          return std::nullopt;
        }
        has_not_null = true;
      } else if (IsWord(token, "DEFAULT")) {
        if (has_default) {
          return FailDuplicate(token, "DEFAULT");
        }
        Advance();
        column.default_value = ParseExpression();
        if (column.default_value == nullptr) {
          return std::nullopt;
        }
        has_default = true;
      } else if (IsWord(token, "REFERENCES")) {
        if (has_references) {
          return FailDuplicate(token, "REFERENCES");
        }
        Advance();
        if (!ExpectIdentifier("a table name", &column.ref_table) ||
            !Expect(TokenKind::kLParen, "'('") ||
            !ExpectIdentifier("a column name", &column.ref_column) ||
            !Expect(TokenKind::kRParen, "')'")) {
          return std::nullopt;
        }
        has_references = true;
      } else {
        break;
      }
    }
    if (IsWord(Current(), "AUTOINCREMENT")) {
      Fail(Current(), "AUTOINCREMENT must follow PRIMARY KEY");
      return std::nullopt;
    }
    return column;
  }

  std::optional<domain::ColumnDefinition> FailDuplicate(
      const Token& token, const std::string& constraint) {
    Fail(token, "duplicate " + constraint);
    return std::nullopt;
  }

  bool ParseOptionalWhere(domain::ExprPtr* where) {
    if (!Match(TokenKind::kWhere)) {
      return true;
    }
    *where = ParseExpression();
    return *where != nullptr;
  }

  bool ParseSelectItems(std::vector<domain::SelectItem>* items) {
    do {
      std::optional<domain::SelectItem> item = ParseSelectItem();
      if (!item.has_value()) {
        return false;
      }
      items->push_back(std::move(*item));
    } while (Match(TokenKind::kComma));
    return true;
  }

  // text is cut from the input between the first and last token of the
  // expression, so it keeps the spelling and spacing as written.
  // `*` is accepted at any position of the list: the Binder rejects the
  // combinations it does not support, such as `SELECT *, COUNT(*)`. A `*`
  // takes no alias.
  std::optional<domain::SelectItem> ParseSelectItem() {
    if (Match(TokenKind::kStar)) {
      return domain::SelectItem{true, nullptr, "", "*"};
    }
    const std::size_t start = Current().offset;
    domain::ExprPtr expr = ParseExpression();
    if (expr == nullptr) {
      return std::nullopt;
    }
    std::string text = input_.substr(start, previous_end_ - start);
    std::string alias;
    if (Match(TokenKind::kAs) && !ExpectIdentifier("alias name", &alias)) {
      return std::nullopt;
    }
    return domain::SelectItem{false, std::move(expr), std::move(alias),
                              std::move(text)};
  }

  std::optional<domain::TableRef> ParseTableRef() {
    domain::TableRef ref;
    if (!ExpectIdentifier("table name", &ref.table)) {
      return std::nullopt;
    }
    if (Match(TokenKind::kAs)) {
      if (!ExpectIdentifier("alias name", &ref.alias)) {
        return std::nullopt;
      }
      return ref;
    }
    if (Current().kind != TokenKind::kIdentifier) {
      return ref;
    }
    if (IsUnsupportedWord(Current()) && Peek(1).kind == TokenKind::kJoin) {
      Fail(Current(), ToUpperAscii(Current().text) + " JOIN is not supported");
      return std::nullopt;
    }
    ref.alias = Current().text;
    Advance();
    return ref;
  }

  bool ParseExpressionList(std::vector<domain::ExprPtr>* expressions) {
    do {
      domain::ExprPtr expr = ParseExpression();
      if (expr == nullptr) {
        return false;
      }
      expressions->push_back(std::move(expr));
    } while (Match(TokenKind::kComma));
    return true;
  }

  bool ParseOrderItems(std::vector<domain::OrderItem>* items) {
    do {
      domain::ExprPtr expr = ParseExpression();
      if (expr == nullptr) {
        return false;
      }
      bool is_descending = false;
      if (!Match(TokenKind::kAsc)) {
        is_descending = Match(TokenKind::kDesc);
      }
      items->push_back(domain::OrderItem{std::move(expr), is_descending});
    } while (Match(TokenKind::kComma));
    return true;
  }

  // Expressions.

  // Entry point for a full expression: statement clauses, parentheses and
  // aggregate arguments all start here, so the nesting bound is checked here.
  domain::ExprPtr ParseExpression() {
    NestingScope scope(&nesting_depth_);
    if (nesting_depth_ > kMaxNestingDepth) {
      return FailTooDeep();
    }
    return ParseOr();
  }

  domain::ExprPtr FailTooDeep() {
    Fail(Current(), "expression is nested too deeply");
    return nullptr;
  }

  // Returns null, with the error recorded, when the node makes the tree
  // deeper than kMaxExpressionDepth.
  domain::ExprPtr CheckDepth(domain::ExprPtr node) {
    if (ExprDepth(*node) > kMaxExpressionDepth) {
      return FailTooDeep();
    }
    return node;
  }

  domain::ExprPtr ParseOr() {
    domain::ExprPtr lhs = ParseAnd();
    while (lhs != nullptr && Match(TokenKind::kOr)) {
      domain::ExprPtr rhs = ParseAnd();
      if (rhs == nullptr) {
        return nullptr;
      }
      lhs = CheckDepth(
          MakeBinary(domain::BinaryOp::kOr, std::move(lhs), std::move(rhs)));
    }
    return lhs;
  }

  domain::ExprPtr ParseAnd() {
    domain::ExprPtr lhs = ParseComparison();
    while (lhs != nullptr && Match(TokenKind::kAnd)) {
      domain::ExprPtr rhs = ParseComparison();
      if (rhs == nullptr) {
        return nullptr;
      }
      lhs = CheckDepth(
          MakeBinary(domain::BinaryOp::kAnd, std::move(lhs), std::move(rhs)));
    }
    return lhs;
  }

  // Comparisons do not chain: `a = b = c` is an error, not `(a = b) = c`.
  domain::ExprPtr ParseComparison() {
    domain::ExprPtr lhs = ParseAdditive();
    if (lhs == nullptr) {
      return nullptr;
    }
    const std::optional<domain::BinaryOp> op =
        ComparisonOperator(Current().kind);
    if (!op.has_value()) {
      return lhs;
    }
    Advance();
    domain::ExprPtr rhs = ParseAdditive();
    if (rhs == nullptr) {
      return nullptr;
    }
    if (ComparisonOperator(Current().kind).has_value()) {
      Fail(Current(),
           "comparison operators cannot be chained; use parentheses");
      return nullptr;
    }
    return CheckDepth(MakeBinary(*op, std::move(lhs), std::move(rhs)));
  }

  // In operator position '-' is always a subtraction, so `1-2` and `1 - -2`
  // both parse; the negative literal is decided in ParseUnary.
  domain::ExprPtr ParseAdditive() {
    domain::ExprPtr lhs = ParseMultiplicative();
    while (lhs != nullptr && (Current().kind == TokenKind::kPlus ||
                              Current().kind == TokenKind::kMinus)) {
      const domain::BinaryOp op = Current().kind == TokenKind::kPlus
                                      ? domain::BinaryOp::kAdd
                                      : domain::BinaryOp::kSub;
      Advance();
      domain::ExprPtr rhs = ParseMultiplicative();
      if (rhs == nullptr) {
        return nullptr;
      }
      lhs = CheckDepth(MakeBinary(op, std::move(lhs), std::move(rhs)));
    }
    return lhs;
  }

  domain::ExprPtr ParseMultiplicative() {
    domain::ExprPtr lhs = ParseConcat();
    while (lhs != nullptr && (Current().kind == TokenKind::kStar ||
                              Current().kind == TokenKind::kSlash)) {
      const domain::BinaryOp op = Current().kind == TokenKind::kStar
                                      ? domain::BinaryOp::kMul
                                      : domain::BinaryOp::kDiv;
      Advance();
      domain::ExprPtr rhs = ParseConcat();
      if (rhs == nullptr) {
        return nullptr;
      }
      lhs = CheckDepth(MakeBinary(op, std::move(lhs), std::move(rhs)));
    }
    return lhs;
  }

  domain::ExprPtr ParseConcat() {
    domain::ExprPtr lhs = ParseUnary();
    while (lhs != nullptr && Match(TokenKind::kConcat)) {
      domain::ExprPtr rhs = ParseUnary();
      if (rhs == nullptr) {
        return nullptr;
      }
      lhs = CheckDepth(MakeBinary(domain::BinaryOp::kConcat, std::move(lhs),
                                  std::move(rhs)));
    }
    return lhs;
  }

  // A '-' directly followed by a number (no blank) is a negative literal; any
  // other '-' lowers to `0 - operand` because the AST has no unary node.
  domain::ExprPtr ParseUnary() {
    if (Current().kind != TokenKind::kMinus) {
      return ParsePrimary();
    }
    NestingScope scope(&nesting_depth_);
    if (nesting_depth_ > kMaxNestingDepth) {
      return FailTooDeep();
    }
    const std::size_t minus_offset = Current().offset;
    const Token& next = Peek(1);
    const bool is_negative_literal =
        next.kind == TokenKind::kNumber && next.offset == minus_offset + 1;
    Advance();
    if (is_negative_literal) {
      return ParseNumber(/*is_negative=*/true, minus_offset);
    }
    domain::ExprPtr operand = ParseUnary();
    if (operand == nullptr) {
      return nullptr;
    }
    return CheckDepth(MakeBinary(domain::BinaryOp::kSub,
                                 MakeLiteral(domain::Value(int64_t{0})),
                                 std::move(operand)));
  }

  // Consumes the current number token. An out-of-range literal is reported at
  // start_offset, the first character of the literal including its sign.
  domain::ExprPtr ParseNumber(bool is_negative, std::size_t start_offset) {
    const Token& token = Current();
    std::optional<domain::Value> value =
        ConvertNumberLiteral(token.text, is_negative);
    if (!value.has_value()) {
      FailAtOffset(start_offset, "numeric literal out of range");
      return nullptr;
    }
    Advance();
    return MakeLiteral(std::move(*value));
  }

  domain::ExprPtr ParsePrimary() {
    const Token& token = Current();
    switch (token.kind) {
      case TokenKind::kNumber:
        return ParseNumber(/*is_negative=*/false, token.offset);
      case TokenKind::kString: {
        domain::ExprPtr literal = MakeLiteral(domain::Value(token.text));
        Advance();
        return literal;
      }
      case TokenKind::kLParen:
        return ParseParenthesized();
      case TokenKind::kCount:
      case TokenKind::kSum:
        return ParseAggregate();
      case TokenKind::kIdentifier:
        // `NOT (` keeps its own message (ParseColumn); any other name directly
        // before '(' is a call.
        if (Peek(1).kind == TokenKind::kLParen &&
            domain::ToLower(token.text) != "not") {
          return ParseCall();
        }
        return ParseColumn();
      case TokenKind::kSelect:
        Fail(token, "subqueries are not supported");
        return nullptr;
      default:
        FailExpected("an expression");
        return nullptr;
    }
  }

  domain::ExprPtr ParseParenthesized() {
    Advance();
    domain::ExprPtr inner = ParseExpression();
    if (inner == nullptr) {
      return nullptr;
    }
    if (!Expect(TokenKind::kRParen, "')'")) {
      return nullptr;
    }
    return inner;
  }

  domain::ExprPtr ParseAggregate() {
    const bool is_count = Current().kind == TokenKind::kCount;
    Advance();
    if (!Expect(TokenKind::kLParen, "'('")) {
      return nullptr;
    }
    domain::ExprPtr arg;
    const bool is_star = is_count && Match(TokenKind::kStar);
    if (!is_star) {
      arg = ParseExpression();
      if (arg == nullptr) {
        return nullptr;
      }
    }
    if (!Expect(TokenKind::kRParen, "')'")) {
      return nullptr;
    }
    const domain::AggregateFn fn =
        is_count ? domain::AggregateFn::kCount : domain::AggregateFn::kSum;
    return CheckDepth(MakeAggregate(fn, std::move(arg)));
  }

  // name "(" ... ")": an aggregate (AVG, MIN, MAX) or a scalar function.
  // Precondition: the current token is an identifier and the next is '('.
  domain::ExprPtr ParseCall() {
    const Token name = Current();
    const std::string upper_name = ToUpperAscii(name.text);
    if (std::optional<domain::AggregateFn> aggregate =
            AggregateNamed(upper_name)) {
      Advance();
      return ParseAggregateArguments(*aggregate);
    }
    std::optional<domain::ScalarFn> function = ScalarFunctionNamed(upper_name);
    if (!function.has_value()) {
      Fail(name, "unknown function " + name.text);
      return nullptr;
    }
    Advance();
    return ParseFunctionArguments(*function);
  }

  // After the aggregate's name: "(" expression ")".
  domain::ExprPtr ParseAggregateArguments(domain::AggregateFn fn) {
    if (!Expect(TokenKind::kLParen, "'('")) {
      return nullptr;
    }
    domain::ExprPtr arg = ParseExpression();
    if (arg == nullptr) {
      return nullptr;
    }
    if (!Expect(TokenKind::kRParen, "')'")) {
      return nullptr;
    }
    return CheckDepth(MakeAggregate(fn, std::move(arg)));
  }

  // After the function's name: "(" arguments ")", or for CAST
  // "(" expression AS type ")". The argument count is the Binder's to check.
  domain::ExprPtr ParseFunctionArguments(domain::ScalarFn fn) {
    if (!Expect(TokenKind::kLParen, "'('")) {
      return nullptr;
    }
    std::vector<domain::ExprPtr> args;
    domain::DataType cast_type = domain::DataType::kText;
    if (fn == domain::ScalarFn::kCast) {
      domain::ExprPtr arg = ParseExpression();
      if (arg == nullptr) {
        return nullptr;
      }
      args.push_back(std::move(arg));
      if (!Expect(TokenKind::kAs, "'AS'")) {
        return nullptr;
      }
      if (!ParseColumnType(&cast_type)) {
        return nullptr;
      }
      if (!Expect(TokenKind::kRParen, "')'")) {
        return nullptr;
      }
    } else {
      if (!ParseExpressionList(&args)) {
        return nullptr;
      }
      if (!Expect(TokenKind::kRParen, "',' or ')'")) {
        return nullptr;
      }
    }
    return CheckDepth(MakeFunction(fn, std::move(args), cast_type));
  }

  // `NOT a = 1` would otherwise lex as the column `NOT` followed by a stray
  // `a`; naming the operator gives a better message.
  domain::ExprPtr ParseColumn() {
    const Token identifier = Current();
    if (domain::ToLower(identifier.text) == "not" &&
        StartsOperand(Peek(1).kind)) {
      Fail(identifier, "NOT is not supported");
      return nullptr;
    }
    Advance();
    if (!Match(TokenKind::kDot)) {
      return MakeColumn("", identifier.text);
    }
    std::string name;
    if (!ExpectIdentifier("column name", &name)) {
      return nullptr;
    }
    return MakeColumn(identifier.text, std::move(name));
  }

  const std::string& input_;
  std::vector<Token> tokens_;
  std::size_t position_ = 0;
  std::size_t previous_end_ = 0;  // end offset of the last consumed token
  std::size_t nesting_depth_ = 0;
  std::optional<ParseError> error_;
};

}  // namespace

namespace {

unsigned char ByteAt(const std::string& text, std::size_t position,
                     std::size_t offset) {
  return static_cast<unsigned char>(text[position + offset]);
}

}  // namespace

std::size_t WhitespaceLengthAt(const std::string& text, std::size_t position) {
  if (position >= text.size()) {
    return 0;
  }
  if (IsSpace(text[position])) {
    return 1;
  }
  const std::size_t remaining = text.size() - position;
  // U+00A0 is C2 A0, U+3000 is E3 80 80, U+2000 to U+200A are E2 80 80 to
  // E2 80 8A.
  if (remaining >= 2 && ByteAt(text, position, 0) == 0xC2 &&
      ByteAt(text, position, 1) == 0xA0) {
    return 2;
  }
  if (remaining >= 3 && ByteAt(text, position, 0) == 0xE3 &&
      ByteAt(text, position, 1) == 0x80 && ByteAt(text, position, 2) == 0x80) {
    return 3;
  }
  if (remaining >= 3 && ByteAt(text, position, 0) == 0xE2 &&
      ByteAt(text, position, 1) == 0x80 && ByteAt(text, position, 2) >= 0x80 &&
      ByteAt(text, position, 2) <= 0x8A) {
    return 3;
  }
  return 0;
}

ParseResult ParseStatement(const std::string& text) {
  Parser parser(text, Tokenize(text));
  return parser.Run();
}

}  // namespace flatfiledb::adapter
