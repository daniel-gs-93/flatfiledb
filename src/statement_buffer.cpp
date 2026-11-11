#include "flatfiledb/statement_buffer.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "flatfiledb/sql_parser.h"

namespace flatfiledb::cli {

namespace {

// U+FF1B in UTF-8.
constexpr const char* kFullWidthSemicolon = "\xEF\xBC\x9B";

bool IsCommentStart(const std::string& line, std::size_t position) {
  return line[position] == '-' && position + 1 < line.size() &&
         line[position + 1] == '-';
}

}  // namespace

bool IsSqlBlank(const std::string& text) {
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t blank = adapter::WhitespaceLengthAt(text, i);
    if (blank == 0) {
      return false;
    }
    i += blank;
  }
  return true;
}

std::string TrimSqlWhitespace(const std::string& text) {
  std::size_t begin = std::string::npos;
  std::size_t end = 0;
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t blank = adapter::WhitespaceLengthAt(text, i);
    if (blank > 0) {
      i += blank;
      continue;
    }
    if (begin == std::string::npos) {
      begin = i;
    }
    ++i;
    end = i;
  }
  if (begin == std::string::npos) {
    return std::string();
  }
  return text.substr(begin, end - begin);
}

std::vector<std::string> StatementBuffer::AddLine(const std::string& line) {
  std::vector<std::string> statements;
  if (!pending_.empty()) {
    pending_ += '\n';
  }
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char letter = line[i];
    if (!is_in_string_ && IsCommentStart(line, i)) {
      // The rest of the line is a comment: dropped when it is all there is of a
      // statement, otherwise kept (the parser skips it) and not scanned.
      if (IsSqlBlank(pending_)) {
        pending_.clear();
      } else {
        pending_ += line.substr(i);
      }
      break;
    }
    pending_ += letter;
    if (letter == '\'') {
      if (is_in_string_ && i + 1 < line.size() && line[i + 1] == '\'') {
        pending_ += '\'';  // a doubled quote is part of the literal
        ++i;
      } else {
        is_in_string_ = !is_in_string_;
      }
    } else if (letter == ';' && !is_in_string_) {
      if (TrimSqlWhitespace(pending_) != ";") {
        statements.push_back(std::move(pending_));
      }
      pending_.clear();
    }
  }
  if (!is_in_string_ && IsSqlBlank(pending_)) {
    pending_.clear();
  }
  return statements;
}

void StatementBuffer::Cancel() {
  pending_.clear();
  is_in_string_ = false;
}

bool StatementBuffer::HasFullWidthSemicolon(const std::string& line) const {
  bool is_in_string = is_in_string_;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (!is_in_string && IsCommentStart(line, i)) {
      return false;
    }
    if (line[i] == '\'') {
      if (is_in_string && i + 1 < line.size() && line[i + 1] == '\'') {
        ++i;
      } else {
        is_in_string = !is_in_string;
      }
    } else if (!is_in_string && line.compare(i, 3, kFullWidthSemicolon) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace flatfiledb::cli
