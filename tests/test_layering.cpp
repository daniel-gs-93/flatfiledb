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
  kPaged,
  kParser,
  kBootstrap,
  kCli,
  kUnknown
};

const char* const kInfrastructureHeaders[] = {
    "flatfiledb/infrastructure.h",     "flatfiledb/flat_file_engine.h",
    "flatfiledb/flat_file_executor.h", "flatfiledb/csv_codec.h",
    "flatfiledb/directory_lock.h",     "flatfiledb/csv_file_source.h"};

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
      "value.cpp", "expression.cpp", "binder.cpp", "scalar_functions.cpp",
      "import_inference.cpp"};
  if (domain_sources.contains(path)) {
    return Layer::kDomain;
  }
  if (path == "flatfiledb/application.h" || path == "application.cpp") {
    return Layer::kApplication;
  }
  if (StartsWith(path, "flatfiledb/paged/") || StartsWith(path, "paged/")) {
    return Layer::kPaged;
  }
  const std::set<std::string> infrastructure_files = {
      "flatfiledb/infrastructure.h",
      "flatfiledb/flat_file_engine.h",
      "flatfiledb/flat_file_executor.h",
      "flatfiledb/csv_codec.h",
      "flatfiledb/directory_lock.h",
      "flatfiledb/csv_file_source.h",
      "infrastructure.cpp",
      "flat_file_engine.cpp",
      "flat_file_executor.cpp",
      "csv_codec.cpp",
      "csv_file_source.cpp",
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
    case Layer::kPaged:
      return StartsWith(header, "flatfiledb/domain/") ||
             StartsWith(header, "flatfiledb/paged/");
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

namespace {

// The byte layer of the paged engine: pure functions on bytes. It may include
// the domain, the shared types and each other, but nothing that does I/O,
// locks or pins (docs/PagedEngineDesignDocument.md 2.1).
bool IsPagedByteLayerFile(const std::string& path) {
  const char* const names[] = {"tuple_codec", "slotted_page", "crc32c"};
  for (const char* name : names) {
    if (path == std::string("flatfiledb/paged/") + name + ".h" ||
        path == std::string("paged/") + name + ".cpp") {
      return true;
    }
  }
  return false;
}

bool IsPagedByteLayerHeader(const std::string& header) {
  const char* const allowed[] = {
      "flatfiledb/paged/paged_types.h", "flatfiledb/paged/byte_codec.h",
      "flatfiledb/paged/crc32c.h", "flatfiledb/paged/slotted_page.h",
      "flatfiledb/paged/tuple_codec.h"};
  for (const char* candidate : allowed) {
    if (header == candidate) {
      return true;
    }
  }
  return false;
}

// Returns how many paged/ files were checked.
int CheckPagedRules(const std::filesystem::path& root) {
  int paged_files = 0;
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
    const std::string path = relative.generic_string();
    const bool is_paged = LayerOf(relative) == Layer::kPaged;
    if (is_paged) {
      ++paged_files;
    }
    std::ifstream file(entry.path());
    std::string line;
    while (std::getline(file, line)) {
      std::string header = ProjectHeaderOf(line);
      if (header.empty()) {
        continue;
      }
      if (!is_paged) {
        if (StartsWith(header, "flatfiledb/paged/")) {
          ::flatfiledb::testing::ReportFailure(__FILE__, __LINE__,
                                               path + " includes " + header);
        }
        continue;
      }
      if (!StartsWith(header, "flatfiledb/domain/") &&
          !StartsWith(header, "flatfiledb/paged/")) {
        ::flatfiledb::testing::ReportFailure(__FILE__, __LINE__,
                                             path + " includes " + header);
      }
      if (IsPagedByteLayerFile(path) &&
          StartsWith(header, "flatfiledb/paged/") &&
          !IsPagedByteLayerHeader(header)) {
        ::flatfiledb::testing::ReportFailure(
            __FILE__, __LINE__,
            path + " is in the byte layer but includes " + header);
      }
    }
  }
  return paged_files;
}

}  // namespace

TEST(test_layering_paged_depends_only_on_domain_and_itself) {
  const std::filesystem::path source_dir = FLATFILEDB_SOURCE_DIR;
  const int paged_files = CheckPagedRules(source_dir / "include") +
                          CheckPagedRules(source_dir / "src");
  // The scan must have found the layer; an empty scan would pass vacuously.
  CHECK(paged_files >= 20);
}
