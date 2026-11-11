#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "test_harness.h"

#ifndef FLATFILEDB_SOURCE_DIR
#error "FLATFILEDB_SOURCE_DIR must be defined by the build"
#endif

namespace {

enum class Layer {
  kDomain,
  kApplication,
  kInfrastructure,
  kParser,
  kBootstrap,
  kCli,
  kUnknown
};

const char* const kInfrastructureHeaders[] = {
    "flatfiledb/infrastructure.h", "flatfiledb/flat_file_engine.h",
    "flatfiledb/flat_file_executor.h", "flatfiledb/csv_codec.h",
    "flatfiledb/directory_lock.h"};

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.starts_with(prefix);
}

bool IsInfrastructureHeader(const std::string& header) {
  for (const char* candidate : kInfrastructureHeaders) {
    if (header == candidate) {
      return true;
    }
  }
  return false;
}

// Layer of a source or header file, by its name relative to include/ or src/.
Layer LayerOf(const std::filesystem::path& relative) {
  const std::string path = relative.generic_string();
  if (StartsWith(path, "flatfiledb/domain/")) {
    return Layer::kDomain;
  }
  const std::set<std::string> domain_sources = {
      "value.cpp", "expression.cpp", "binder.cpp", "scalar_functions.cpp"};
  if (domain_sources.contains(path)) {
    return Layer::kDomain;
  }
  if (path == "flatfiledb/application.h" || path == "application.cpp") {
    return Layer::kApplication;
  }
  const std::set<std::string> infrastructure_files = {
      "flatfiledb/infrastructure.h",
      "flatfiledb/flat_file_engine.h",
      "flatfiledb/flat_file_executor.h",
      "flatfiledb/csv_codec.h",
      "flatfiledb/directory_lock.h",
      "infrastructure.cpp",
      "flat_file_engine.cpp",
      "flat_file_executor.cpp",
      "csv_codec.cpp",
      "directory_lock.cpp"};
  if (infrastructure_files.contains(path)) {
    return Layer::kInfrastructure;
  }
  if (path == "flatfiledb/sql_parser.h" || path == "sql_parser.cpp") {
    return Layer::kParser;
  }
  if (path == "flatfiledb/bootstrap.h" || path == "bootstrap.cpp") {
    return Layer::kBootstrap;
  }
  const std::set<std::string> cli_files = {
      "cli.cpp", "repl.cpp", "statement_buffer.cpp", "flatfiledb/repl.h",
      "flatfiledb/statement_buffer.h"};
  if (cli_files.contains(path)) {
    return Layer::kCli;
  }
  return Layer::kUnknown;
}

// The header named by an `#include "flatfiledb/..."` line, or empty.
std::string ProjectHeaderOf(const std::string& line) {
  const std::string marker = "#include \"flatfiledb/";
  if (!StartsWith(line, marker)) {
    return "";
  }
  std::size_t begin = std::string("#include \"").size();
  std::size_t end = line.find('"', begin);
  if (end == std::string::npos) {
    return "";
  }
  return line.substr(begin, end - begin);
}

bool IsAllowed(Layer layer, const std::string& header) {
  switch (layer) {
    case Layer::kDomain:
      return StartsWith(header, "flatfiledb/domain/");
    case Layer::kApplication:
      return StartsWith(header, "flatfiledb/domain/") ||
             header == "flatfiledb/application.h";
    case Layer::kInfrastructure:
      return StartsWith(header, "flatfiledb/domain/") ||
             IsInfrastructureHeader(header);
    case Layer::kParser:
      return StartsWith(header, "flatfiledb/domain/") ||
             header == "flatfiledb/sql_parser.h";
    case Layer::kBootstrap:
      // The composition root: the only file outside Infrastructure that may
      // name infrastructure types.
      return StartsWith(header, "flatfiledb/domain/") ||
             header == "flatfiledb/application.h" ||
             header == "flatfiledb/bootstrap.h" ||
             IsInfrastructureHeader(header);
    case Layer::kCli:
      return StartsWith(header, "flatfiledb/domain/") ||
             header == "flatfiledb/application.h" ||
             header == "flatfiledb/bootstrap.h" ||
             header == "flatfiledb/sql_parser.h" ||
             header == "flatfiledb/repl.h" ||
             header == "flatfiledb/statement_buffer.h";
    case Layer::kUnknown:
      return false;
  }
  return false;
}

void CheckDirectory(const std::filesystem::path& root) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string extension = entry.path().extension().string();
    if (extension != ".h" && extension != ".cpp") {
      continue;
    }
    std::filesystem::path relative =
        std::filesystem::relative(entry.path(), root);
    Layer layer = LayerOf(relative);
    if (layer == Layer::kUnknown) {
      ::flatfiledb::testing::ReportFailure(
          __FILE__, __LINE__,
          "file has no layer assignment: " + relative.string());
      continue;
    }
    std::ifstream file(entry.path());
    if (!file.is_open()) {
      ::flatfiledb::testing::ReportFailure(__FILE__, __LINE__,
                                           "cannot read " + relative.string());
      continue;
    }
    std::string line;
    while (std::getline(file, line)) {
      std::string header = ProjectHeaderOf(line);
      if (header.empty() || IsAllowed(layer, header)) {
        continue;
      }
      ::flatfiledb::testing::ReportFailure(
          __FILE__, __LINE__, relative.string() + " includes " + header);
    }
  }
}

}  // namespace

TEST(test_layering_includes_follow_dependency_direction) {
  const std::filesystem::path source_dir = FLATFILEDB_SOURCE_DIR;
  CheckDirectory(source_dir / "include");
  CheckDirectory(source_dir / "src");
}
