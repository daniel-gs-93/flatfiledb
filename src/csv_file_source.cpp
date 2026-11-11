#include "flatfiledb/csv_file_source.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/csv_codec.h"

namespace flatfiledb::infrastructure {

namespace {

constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";

}  // namespace

std::variant<std::vector<domain::CsvLine>, std::string> FileCsvSource::Read(
    const std::string& path) {
  const std::string failure = "cannot read " + path;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return failure;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    return failure;
  }

  std::vector<domain::CsvLine> lines;
  std::string line;
  std::size_t number = 0;
  while (std::getline(file, line)) {
    ++number;
    if (number == 1 && line.starts_with(kUtf8Bom)) {
      line.erase(0, 3);
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    lines.push_back(domain::CsvLine{number, ParseCsvRecord(line)});
  }
  if (file.bad()) {
    return failure;
  }
  return lines;
}

}  // namespace flatfiledb::infrastructure
