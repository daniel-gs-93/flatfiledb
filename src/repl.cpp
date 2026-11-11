#include "flatfiledb/repl.h"

#include <algorithm>
#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/application.h"
#include "flatfiledb/bootstrap.h"
#include "flatfiledb/domain/value.h"
#include "flatfiledb/sql_parser.h"
#include "flatfiledb/statement_buffer.h"

namespace flatfiledb::cli {

namespace {

constexpr const char* kPrompt = "flatfiledb> ";
constexpr const char* kContinuationPrompt = "       ...> ";
constexpr const char* kNoDatabase =
    "error: no database open; run .open first\n";
constexpr const char* kHelp =
    ".open <directory> [--parallel]  open a database directory\n"
    ".tables                         list the tables\n"
    ".schema [<table>]               show the CREATE TABLE statements\n"
    ".help                           show this text\n"
    ".quit                           leave (also .exit)\n"
    "Statements end with ';' and may span several lines.\n";

using Database = std::optional<bootstrap::OpenedDatabase>;

std::string UsageError(const std::string& usage) {
  return "syntax error at column 1: usage: " + usage + "\n";
}

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
std::string OpenDatabase(const OpenArguments& arguments, Database& database) {
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

std::string ExecuteSql(const std::string& statement_text, Database& database) {
  adapter::ParseResult parsed = adapter::ParseStatement(statement_text);
  if (const auto* error = std::get_if<adapter::ParseError>(&parsed)) {
    return FormatParseError(statement_text, *error) + "\n";
  }
  if (!database) {
    return kNoDatabase;
  }
  return RenderResult(
      database->service->Execute(std::get<domain::Statement>(parsed)));
}

std::string ListTables(Database& database) {
  std::string text;
  for (const domain::TableSchema& schema :
       database->service->DescribeCatalog()) {
    text += schema.name + "\n";
  }
  return text;
}

// One table when `table` is non-empty, otherwise every table, in catalog
// order.
std::string ShowSchema(const std::string& table, Database& database) {
  const std::vector<domain::TableSchema> catalog =
      database->service->DescribeCatalog();
  std::string text;
  bool is_found = false;
  for (const domain::TableSchema& schema : catalog) {
    if (!table.empty() &&
        domain::ToLower(schema.name) != domain::ToLower(table)) {
      continue;
    }
    is_found = true;
    text += FormatCreateTable(schema, catalog) + "\n";
  }
  if (!table.empty() && !is_found) {
    return "error: no such table: " + table + "\n";
  }
  return text;
}

// The result of a line that holds a command.
struct CommandOutcome {
  bool is_command;  // False: the line is not a command and holds SQL.
  std::string text;
  bool should_quit;
};

CommandOutcome Text(std::string text) {
  return CommandOutcome{true, std::move(text), false};
}

// Runs the line when its first token is a command; otherwise reports that it is
// not one, so that the line is read as SQL (an unknown ".foo" is then a syntax
// error from the parser).
CommandOutcome HandleCommand(const std::string& line, Database& database) {
  std::vector<std::string> tokens = SplitWhitespace(line);
  if (tokens.empty()) {
    return CommandOutcome{false, "", false};
  }
  const std::string& name = tokens[0];
  if (name == ".open") {
    std::optional<OpenArguments> arguments = ParseOpenArguments(tokens);
    if (!arguments) {
      return Text(UsageError(".open <directory> [--parallel]"));
    }
    return Text(OpenDatabase(*arguments, database));
  }
  if (name == ".tables") {
    if (tokens.size() != 1) {
      return Text(UsageError(".tables"));
    }
    return Text(database ? ListTables(database) : kNoDatabase);
  }
  if (name == ".schema") {
    if (tokens.size() > 2) {
      return Text(UsageError(".schema [<table>]"));
    }
    if (!database) {
      return Text(kNoDatabase);
    }
    return Text(
        ShowSchema(tokens.size() == 2 ? tokens[1] : std::string(), database));
  }
  if (name == ".help") {
    return Text(tokens.size() == 1 ? std::string(kHelp) : UsageError(".help"));
  }
  if (name == ".quit" || name == ".exit") {
    if (tokens.size() != 1) {
      return Text(UsageError(".quit"));
    }
    return CommandOutcome{true, "", true};
  }
  return CommandOutcome{false, "", false};
}

std::string SqlTypeName(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      return "TEXT";
  }
  return "";
}

// A default value as an SQL literal: TEXT in single quotes, a quote written
// twice.
std::string SqlLiteral(const domain::Value& value) {
  const auto* text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    return domain::FormatValue(value);
  }
  std::string literal = "'";
  for (char letter : *text) {
    if (letter == '\'') {
      literal += '\'';
    }
    literal += letter;
  }
  return literal + "'";
}

const domain::TableSchema* FindSchema(
    const std::vector<domain::TableSchema>& catalog, const std::string& name) {
  for (const domain::TableSchema& schema : catalog) {
    if (domain::ToLower(schema.name) == domain::ToLower(name)) {
      return &schema;
    }
  }
  return nullptr;
}

std::string ColumnConstraints(const domain::TableSchema& schema,
                              std::size_t column_index,
                              const std::vector<domain::TableSchema>& catalog) {
  std::string text;
  if (column_index == schema.primary_key_index) {
    text += " PRIMARY KEY";
    if (schema.auto_increment_index == column_index) {
      text += " AUTOINCREMENT";
    }
  }
  if (std::ranges::find(schema.unique_indexes, column_index) !=
      schema.unique_indexes.end()) {
    text += " UNIQUE";
  }
  const domain::ColumnDef& column = schema.columns[column_index];
  if (column.default_value) {
    text += " DEFAULT " + SqlLiteral(*column.default_value);
  }
  for (const domain::ForeignKey& foreign_key : schema.foreign_keys) {
    if (foreign_key.column_index != column_index) {
      continue;
    }
    const domain::TableSchema* target =
        FindSchema(catalog, foreign_key.ref_table);
    text +=
        " REFERENCES " + foreign_key.ref_table + "(" +
        (target != nullptr ? target->columns[foreign_key.ref_column_index].name
                           : std::to_string(foreign_key.ref_column_index)) +
        ")";
  }
  return text;
}

}  // namespace

std::string FormatCreateTable(const domain::TableSchema& schema,
                              const std::vector<domain::TableSchema>& catalog) {
  std::string text = "CREATE TABLE " + schema.name + " (\n";
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    text += "  " + schema.columns[i].name + " " +
            SqlTypeName(schema.columns[i].type) +
            ColumnConstraints(schema, i, catalog);
    text += (i + 1 < schema.columns.size()) ? ",\n" : "\n";
  }
  return text + ");";
}

std::string FormatParseError(const std::string& statement,
                             const adapter::ParseError& error) {
  const std::size_t offset = std::min(
      error.column == 0 ? std::size_t{0} : error.column - 1, statement.size());
  const std::size_t newlines = static_cast<std::size_t>(
      std::count(statement.begin(), statement.begin() + offset, '\n'));
  if (newlines == 0) {
    return "syntax error at column " + std::to_string(error.column) + ": " +
           error.description;
  }
  const std::size_t line_start = statement.rfind('\n', offset - 1);
  return "syntax error at line " + std::to_string(newlines + 1) + ", column " +
         std::to_string(offset - line_start) + ": " + error.description;
}

void RunRepl(std::istream& input, std::ostream& output, bool is_interactive) {
  Database database;
  StatementBuffer buffer;
  std::string line;
  while (true) {
    if (is_interactive) {
      output << (buffer.IsEmpty() ? kPrompt : kContinuationPrompt)
             << std::flush;
    }
    if (!std::getline(input, line)) {
      break;
    }
    line = StripTrailingCarriageReturn(std::move(line));
    if (buffer.IsEmpty()) {
      if (IsBlank(line)) {
        continue;
      }
      CommandOutcome command = HandleCommand(line, database);
      if (command.is_command) {
        output << command.text;
        if (command.should_quit) {
          break;
        }
        continue;
      }
    }
    for (const std::string& statement : buffer.AddLine(line)) {
      output << ExecuteSql(statement, database);
    }
  }
  if (!buffer.IsEmpty()) {
    output << "error: incomplete statement at end of input\n";
  }
}

}  // namespace flatfiledb::cli
