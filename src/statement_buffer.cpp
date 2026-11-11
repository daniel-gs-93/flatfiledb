#include "flatfiledb/statement_buffer.h"

#include <string>
#include <utility>
#include <vector>

namespace flatfiledb::cli {

namespace {

bool IsBlank(const std::string& text) {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// The text without leading and trailing white space.
std::string Trimmed(const std::string& text) {
  std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

}  // namespace

std::vector<std::string> StatementBuffer::AddLine(const std::string& line) {
  std::vector<std::string> statements;
  if (!pending_.empty()) {
    pending_ += '\n';
  }
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char letter = line[i];
    pending_ += letter;
    if (letter == '\'') {
      if (is_in_string_ && i + 1 < line.size() && line[i + 1] == '\'') {
        pending_ += '\'';  // a doubled quote is part of the literal
        ++i;
      } else {
        is_in_string_ = !is_in_string_;
      }
    } else if (letter == ';' && !is_in_string_) {
      if (Trimmed(pending_) != ";") {
        statements.push_back(std::move(pending_));
      }
      pending_.clear();
    }
  }
  if (!is_in_string_ && IsBlank(pending_)) {
    pending_.clear();
  }
  return statements;
}

}  // namespace flatfiledb::cli
