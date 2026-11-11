#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/bootstrap.h"
#include "flatfiledb/domain/value.h"
#include "flatfiledb/sql_parser.h"

namespace flatfiledb::cli {

namespace {

constexpr const char* kOpenUsage =
    "syntax error at column 1: usage: .open <directory> [--parallel]\n";

struct OpenArguments {
  std::string directory;
  bool is_parallel;
};

std::vector<std::string> SplitWhitespace(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

std::string StripTrailingCarriageReturn(std::string line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

bool IsBlank(const std::string& line) {
  return line.find_first_not_of(" \t") == std::string::npos;
}

// `.open <directory> [--parallel]`; std::nullopt when the arguments do not
// match.
std::optional<OpenArguments> ParseOpenArguments(
    const std::vector<std::string>& tokens) {
  if (tokens.size() != 2 && tokens.size() != 3) {
    return std::nullopt;
  }
  if (tokens.size() == 3 && tokens[2] != "--parallel") {
    return std::nullopt;
  }
  return OpenArguments{tokens[1], tokens.size() == 3};
}

std::string JoinWithTabs(const std::vector<std::string>& cells) {
  std::string line;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (i > 0) {
      line += '\t';
    }
    line += cells[i];
  }
  return line;
}

std::string RenderResultSet(const domain::ResultSet& result_set) {
  std::vector<std::string> header;
  for (const auto& column : result_set.columns) {
    header.push_back(column.name);
  }
  std::string text = JoinWithTabs(header) + "\n";
  for (const auto& row : result_set.rows) {
    std::vector<std::string> cells;
    for (const auto& value : row) {
      cells.push_back(domain::FormatValue(value));
    }
    text += JoinWithTabs(cells) + "\n";
  }
  return text;
}

// Text for one execution result, ending with a newline.
std::string RenderResult(const domain::ExecutionResult& result) {
  if (const auto* result_set = std::get_if<domain::ResultSet>(&result)) {
    return RenderResultSet(*result_set);
  }
  if (const auto* affected = std::get_if<domain::AffectedRows>(&result)) {
    return "OK affected=" + std::to_string(affected->count) + "\n";
  }
  if (const auto* complete = std::get_if<domain::CommandComplete>(&result)) {
    return "OK " + complete->tag + "\n";
  }
  const auto& error = std::get<domain::ExecutionError>(result);
  return std::string("error: ") + domain::ErrorKindName(error.kind) + ": " +
         error.message + "\n";
}

// `users=10000 posts=10000`, or `no tables`.
std::string DescribeTables(const std::vector<bootstrap::TableCount>& tables) {
  if (tables.empty()) {
    return "no tables";
  }
  std::string text;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (i > 0) {
      text += ' ';
    }
    text += tables[i].name + "=" + std::to_string(tables[i].row_count);
  }
  return text;
}

// Opens the database named by `arguments` and returns the line to print. A
// successful open replaces `database`; a failed one leaves it untouched.
std::string OpenDatabase(const OpenArguments& arguments,
                         std::optional<bootstrap::OpenedDatabase>& database) {
  auto opened = bootstrap::OpenFlatFileDatabase(
      arguments.directory, arguments.is_parallel,
      database ? &*database : nullptr);
  if (const auto* reason = std::get_if<std::string>(&opened)) {
    return "error: could not open " + arguments.directory + ": " + *reason +
           "\n";
  }
  database = std::move(std::get<bootstrap::OpenedDatabase>(opened));
  return "loaded " + DescribeTables(database->tables) + "\n";
}

std::string ExecuteSql(const std::string& line,
                       application::QueryService* service) {
  adapter::ParseResult parsed = adapter::ParseStatement(line);
  if (const auto* error = std::get_if<adapter::ParseError>(&parsed)) {
    return "syntax error at column " + std::to_string(error->column) + ": " +
           error->description + "\n";
  }
  if (service == nullptr) {
    return "error: no database open; run .open first\n";
  }
  return RenderResult(service->Execute(std::get<domain::Statement>(parsed)));
}

// Handles one input line and returns the text to print.
std::string HandleLine(const std::string& line,
                       std::optional<bootstrap::OpenedDatabase>& database) {
  std::vector<std::string> tokens = SplitWhitespace(line);
  if (tokens.empty() || tokens[0] != ".open") {
    return ExecuteSql(line, database ? database->service.get() : nullptr);
  }
  std::optional<OpenArguments> arguments = ParseOpenArguments(tokens);
  if (!arguments) {
    return kOpenUsage;
  }
  return OpenDatabase(*arguments, database);
}

// Reads one command or statement per line until end of input; the only
// function that touches the streams.
void RunRepl(std::istream& input, std::ostream& output) {
  std::optional<bootstrap::OpenedDatabase> database;
  std::string line;
  while (std::getline(input, line)) {
    line = StripTrailingCarriageReturn(std::move(line));
    if (IsBlank(line)) {
      continue;
    }
    output << HandleLine(line, database);
  }
}

}  // namespace

}  // namespace flatfiledb::cli

int main() {
  flatfiledb::cli::RunRepl(std::cin, std::cout);
  return 0;
}
