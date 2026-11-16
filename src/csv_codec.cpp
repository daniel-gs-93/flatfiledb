#include "strata/csv_codec.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace strata::infrastructure {

namespace {

constexpr const char* kCatalogHeader =
    "table,position,column,type,primary_key,unique,auto_increment,has_default,"
    "default,ref_table,ref_column";
constexpr std::size_t kCatalogFieldCount = 11;
constexpr const char* kReservedTableName = "catalog";

bool TryParseInt(const std::string& text, int64_t& parsed) {
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, parsed);
  return result.ec == std::errc() && result.ptr == end;
}

// strtod accepts white space, a '+' sign, hexadecimal and "inf"; only a plain
// decimal number is a DOUBLE in a CSV file.
bool TryParseDouble(const std::string& text, double& parsed) {
  if (text.empty() || text.front() == '+') {
    return false;
  }
  if (text.find_first_not_of("0123456789-.eE") != std::string::npos) {
    return false;
  }
  char* end = nullptr;
  parsed = std::strtod(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(parsed);
}

std::optional<domain::Value> ParseValue(const std::string& field,
                                        domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt: {
      int64_t integer = 0;
      if (!TryParseInt(field, integer)) {
        return std::nullopt;
      }
      return domain::Value(integer);
    }
    case domain::DataType::kDouble: {
      double number = 0.0;
      if (!TryParseDouble(field, number)) {
        return std::nullopt;
      }
      return domain::Value(number);
    }
    case domain::DataType::kText:
      return domain::Value(field);
  }
  return std::nullopt;
}

const char* TypeName(domain::DataType type) {
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

std::optional<domain::DataType> ParseTypeName(const std::string& name) {
  if (name == "INT") {
    return domain::DataType::kInt;
  }
  if (name == "DOUBLE") {
    return domain::DataType::kDouble;
  }
  if (name == "TEXT") {
    return domain::DataType::kText;
  }
  return std::nullopt;
}

std::string Flag(bool is_set) { return is_set ? "1" : "0"; }

std::string JoinFields(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      line += ',';
    }
    line += FormatCsvField(fields[i]);
  }
  return line;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    begin = end + 1;
  }
  return lines;
}

// One row of catalog.csv, before the schemas are assembled.
struct CatalogRow {
  std::size_t line_number;
  std::string table;
  std::size_t position;
  std::string column;
  domain::DataType type;
  bool is_primary_key;
  bool is_unique;
  bool is_auto_increment;
  std::optional<domain::Value> default_value;
  std::string ref_table;
  std::string ref_column;
};

std::string LinePrefix(std::size_t line_number) {
  return "line " + std::to_string(line_number) + ": ";
}

bool TryParseFlag(const std::string& text, bool& flag) {
  if (text != "0" && text != "1") {
    return false;
  }
  flag = text == "1";
  return true;
}

// Fills row from the fields of one catalog line; returns a description on
// failure.
std::optional<std::string> ParseCatalogRow(
    const std::vector<std::string>& fields, std::size_t line_number,
    CatalogRow* row) {
  const std::string where = LinePrefix(line_number);
  if (fields.size() != kCatalogFieldCount) {
    return where + "expected " + std::to_string(kCatalogFieldCount) +
           " fields, found " + std::to_string(fields.size());
  }
  row->line_number = line_number;
  row->table = fields[0];
  row->column = fields[2];
  if (row->table.empty() || row->column.empty()) {
    return where + "table and column names must not be empty";
  }
  int64_t position = 0;
  if (!TryParseInt(fields[1], position) || position < 0) {
    return where + "position '" + fields[1] + "' is not a number";
  }
  row->position = static_cast<std::size_t>(position);
  std::optional<domain::DataType> type = ParseTypeName(fields[3]);
  if (!type) {
    return where + "unknown type '" + fields[3] + "'";
  }
  row->type = *type;
  bool has_default = false;
  if (!TryParseFlag(fields[4], row->is_primary_key) ||
      !TryParseFlag(fields[5], row->is_unique) ||
      !TryParseFlag(fields[6], row->is_auto_increment) ||
      !TryParseFlag(fields[7], has_default)) {
    return where + "the flags must be 0 or 1";
  }
  if (has_default) {
    row->default_value = ParseValue(fields[8], row->type);
    if (!row->default_value) {
      return where + "default '" + fields[8] + "' of column " + row->column +
             " does not match its type";
    }
  } else if (!fields[8].empty()) {
    return where + "column " + row->column + " has a default but no flag";
  }
  row->ref_table = fields[9];
  row->ref_column = fields[10];
  if (row->ref_table.empty() != row->ref_column.empty()) {
    return where + "ref_table and ref_column go together";
  }
  return std::nullopt;
}

// The checks on the columns of one table that need nothing from other tables.
std::optional<std::string> CheckTable(const std::vector<CatalogRow>& rows) {
  const std::string& table = rows.front().table;
  if (domain::ToLower(table) == kReservedTableName) {
    return "table name " + table + " is reserved";
  }
  std::set<std::string> names;
  std::size_t primary_keys = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const CatalogRow& row = rows[i];
    if (row.position != i) {
      return "columns of table " + table + " are not numbered 0, 1, 2, ...";
    }
    if (!names.insert(domain::ToLower(row.column)).second) {
      return "column " + row.column + " of table " + table +
             " is defined twice";
    }
    if (row.is_primary_key) {
      ++primary_keys;
    }
    if (row.is_primary_key && (row.is_unique || row.default_value)) {
      return "primary key " + row.column + " of table " + table +
             " cannot be marked unique or have a default";
    }
    if (row.is_auto_increment &&
        (!row.is_primary_key || row.type != domain::DataType::kInt)) {
      return "auto-increment column " + row.column + " of table " + table +
             " must be an INT primary key";
    }
  }
  if (primary_keys != 1) {
    return "table " + table + " must have exactly one primary key";
  }
  return std::nullopt;
}

domain::TableSchema BuildSchema(const std::vector<CatalogRow>& rows) {
  domain::TableSchema schema;
  schema.name = rows.front().table;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const CatalogRow& row = rows[i];
    schema.columns.push_back(
        domain::ColumnDef{row.column, row.type, row.default_value});
    if (row.is_primary_key) {
      schema.primary_key_index = i;
    } else if (row.is_unique) {
      schema.unique_indexes.push_back(i);
    }
    if (row.is_auto_increment) {
      schema.auto_increment_index = i;
    }
  }
  return schema;
}

// Resolves the REFERENCES of every table once all tables are known.
std::optional<std::string> ResolveForeignKeys(
    const std::vector<std::vector<CatalogRow>>& tables,
    std::vector<domain::TableSchema>* schemas) {
  for (std::size_t table_index = 0; table_index < tables.size();
       ++table_index) {
    for (std::size_t i = 0; i < tables[table_index].size(); ++i) {
      const CatalogRow& row = tables[table_index][i];
      if (row.ref_table.empty()) {
        continue;
      }
      const std::string where =
          "column " + row.column + " of table " + row.table + " references ";
      const domain::TableSchema* target = nullptr;
      for (const domain::TableSchema& candidate : *schemas) {
        if (domain::ToLower(candidate.name) == domain::ToLower(row.ref_table)) {
          target = &candidate;
        }
      }
      if (target == nullptr || target == &(*schemas)[table_index]) {
        return where + "an unknown table " + row.ref_table;
      }
      std::size_t target_index = target->columns.size();
      for (std::size_t column_index = 0; column_index < target->columns.size();
           ++column_index) {
        if (domain::ToLower(target->columns[column_index].name) ==
            domain::ToLower(row.ref_column)) {
          target_index = column_index;
        }
      }
      if (target_index == target->columns.size()) {
        return where + "an unknown column " + row.ref_table + "." +
               row.ref_column;
      }
      if (target->columns[target_index].type != row.type) {
        return where + target->name + "." + row.ref_column + " of another type";
      }
      bool is_unique =
          target_index == target->primary_key_index ||
          std::ranges::find(target->unique_indexes, target_index) !=
              target->unique_indexes.end();
      if (!is_unique) {
        return where + target->name + "." + row.ref_column +
               ", which is neither a primary key nor unique";
      }
      (*schemas)[table_index].foreign_keys.push_back(
          domain::ForeignKey{i, target->name, target_index});
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::vector<std::string>> ParseCsvRecord(
    const std::string& line) {
  std::vector<std::string> fields;
  std::size_t i = 0;
  while (true) {
    std::string field;
    if (i < line.size() && line[i] == '"') {
      ++i;
      bool is_closed = false;
      while (i < line.size()) {
        if (line[i] != '"') {
          field += line[i++];
        } else if (i + 1 < line.size() && line[i + 1] == '"') {
          field += '"';
          i += 2;
        } else {
          ++i;
          is_closed = true;
          break;
        }
      }
      if (!is_closed) {
        return std::nullopt;
      }
      if (i < line.size() && line[i] != ',') {
        return std::nullopt;
      }
    } else {
      while (i < line.size() && line[i] != ',') {
        field += line[i++];
      }
    }
    fields.push_back(std::move(field));
    if (i >= line.size()) {
      break;
    }
    ++i;  // the ','
    if (i == line.size()) {
      fields.push_back(std::string());
      break;
    }
  }
  return fields;
}

std::string FormatCsvField(const std::string& field) {
  if (field.find_first_of(",\"") == std::string::npos) {
    return field;
  }
  std::string quoted = "\"";
  for (char letter : field) {
    if (letter == '"') {
      quoted += '"';
    }
    quoted += letter;
  }
  quoted += '"';
  return quoted;
}

std::string SerializeRow(const domain::Row& row) {
  // A lone empty field would make a blank line, which the loaders skip.
  if (row.size() == 1 && domain::FormatValue(row[0]).empty()) {
    return "\"\"";
  }
  std::string line;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i > 0) {
      line += ',';
    }
    line += FormatCsvField(domain::FormatValue(row[i]));
  }
  return line;
}

std::optional<domain::Row> ParseRow(const std::vector<std::string>& fields,
                                    const domain::TableSchema& schema) {
  if (fields.size() != schema.columns.size()) {
    return std::nullopt;
  }
  domain::Row row;
  row.reserve(fields.size());
  for (std::size_t i = 0; i < fields.size(); ++i) {
    std::optional<domain::Value> value =
        ParseValue(fields[i], schema.columns[i].type);
    if (!value) {
      return std::nullopt;
    }
    row.push_back(std::move(*value));
  }
  return row;
}

std::string HeaderLine(const domain::TableSchema& schema) {
  std::string header;
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (i > 0) {
      header += ',';
    }
    header += schema.columns[i].name;
  }
  return header;
}

bool HeaderMatches(const std::vector<std::string>& fields,
                   const domain::TableSchema& schema) {
  if (fields.size() != schema.columns.size()) {
    return false;
  }
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (domain::ToLower(fields[i]) != domain::ToLower(schema.columns[i].name)) {
      return false;
    }
  }
  return true;
}

std::string SerializeCatalog(const std::vector<domain::TableSchema>& schemas) {
  std::string text = std::string(kCatalogHeader) + "\n";
  for (const domain::TableSchema& schema : schemas) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      const domain::ColumnDef& column = schema.columns[i];
      std::string ref_table;
      std::string ref_column;
      for (const domain::ForeignKey& foreign_key : schema.foreign_keys) {
        if (foreign_key.column_index != i) {
          continue;
        }
        // A foreign key holds the target's column position; the file holds its
        // name, which only the target's schema has.
        for (const domain::TableSchema& target : schemas) {
          if (domain::ToLower(target.name) !=
              domain::ToLower(foreign_key.ref_table)) {
            continue;
          }
          ref_table = target.name;
          ref_column = target.columns[foreign_key.ref_column_index].name;
        }
      }
      bool is_unique = std::ranges::find(schema.unique_indexes, i) !=
                       schema.unique_indexes.end();
      std::string default_text =
          column.default_value ? domain::FormatValue(*column.default_value)
                               : std::string();
      text += JoinFields({schema.name, std::to_string(i), column.name,
                          TypeName(column.type),
                          Flag(i == schema.primary_key_index), Flag(is_unique),
                          Flag(schema.auto_increment_index == i),
                          Flag(column.default_value.has_value()), default_text,
                          ref_table, ref_column}) +
              "\n";
    }
  }
  return text;
}

std::variant<std::vector<domain::TableSchema>, std::string> ParseCatalog(
    const std::string& text) {
  std::vector<std::string> lines = SplitLines(text);
  std::size_t index = 0;
  while (index < lines.size() && lines[index].empty()) {
    ++index;
  }
  if (index == lines.size()) {
    return std::string("the file has no header");
  }
  if (lines[index] != kCatalogHeader) {
    return LinePrefix(index + 1) + "the header is not " + kCatalogHeader;
  }

  std::vector<std::vector<CatalogRow>> tables;
  std::set<std::string> seen_tables;
  for (++index; index < lines.size(); ++index) {
    if (lines[index].empty()) {
      continue;
    }
    const std::size_t line_number = index + 1;
    std::optional<std::vector<std::string>> fields =
        ParseCsvRecord(lines[index]);
    if (!fields) {
      return LinePrefix(line_number) + "unbalanced quotes";
    }
    CatalogRow row;
    if (auto error = ParseCatalogRow(*fields, line_number, &row)) {
      return *error;
    }
    if (tables.empty() || tables.back().front().table != row.table) {
      if (!seen_tables.insert(domain::ToLower(row.table)).second) {
        return LinePrefix(line_number) + "table " + row.table +
               " is defined twice";
      }
      tables.emplace_back();
    }
    tables.back().push_back(std::move(row));
  }

  std::vector<domain::TableSchema> schemas;
  for (const std::vector<CatalogRow>& rows : tables) {
    if (auto error = CheckTable(rows)) {
      return *error;
    }
    schemas.push_back(BuildSchema(rows));
  }
  if (auto error = ResolveForeignKeys(tables, &schemas)) {
    return *error;
  }
  return schemas;
}

}  // namespace strata::infrastructure
