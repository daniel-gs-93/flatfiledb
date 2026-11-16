#ifndef STRATA_CSV_CODEC_H_
#define STRATA_CSV_CODEC_H_

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/value.h"

// Pure text conversion for the flatfile engine: one CSV line (RFC 4180, with no
// line break inside a field) and the catalog file. No file access.

namespace strata::infrastructure {

// Splits one line, without its line ending, into fields. A field starting with
// '"' is quoted: it ends at the next single '"', and a doubled '"' inside is
// one '"'. A '"' inside an unquoted field is kept as it is. std::nullopt when a
// quote is not closed or text follows a closing quote.
std::optional<std::vector<std::string>> ParseCsvRecord(const std::string& line);

// The field as it is written: quoted when it holds ',' or '"', unchanged
// otherwise.
std::string FormatCsvField(const std::string& field);

// The row as one CSV line without a line ending; each value is written with
// FormatValue and then FormatCsvField. A row whose only value is empty is
// written as "" so that the line is not blank.
std::string SerializeRow(const domain::Row& row);

// The fields as a row of the schema's column types. std::nullopt when the field
// count differs from the column count, an INT is not a decimal number in the
// int64 range, or a DOUBLE is not a finite decimal number. A leading '+' or
// white space is rejected.
std::optional<domain::Row> ParseRow(const std::vector<std::string>& fields,
                                    const domain::TableSchema& schema);

// The header line of a table file: the column names joined by ','.
std::string HeaderLine(const domain::TableSchema& schema);

// Whether the header fields name the schema's columns in order, compared
// case-insensitively.
bool HeaderMatches(const std::vector<std::string>& fields,
                   const domain::TableSchema& schema);

// The text of catalog.csv for the schemas in the given order (the format of
// docs/DesignDocument.md section 2.6), lines ending in "\n".
std::string SerializeCatalog(const std::vector<domain::TableSchema>& schemas);

// The schemas of catalog.csv, or the description of the first problem. Checks
// everything docs/DesignDocument.md section 2.6 lists.
std::variant<std::vector<domain::TableSchema>, std::string> ParseCatalog(
    const std::string& text);

}  // namespace strata::infrastructure

#endif  // STRATA_CSV_CODEC_H_
