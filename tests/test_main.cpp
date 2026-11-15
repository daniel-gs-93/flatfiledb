#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "test_harness.h"

namespace strata::testing {

namespace {

struct CurrentTest {
  int failure_count = 0;
  bool is_skipped = false;
  std::string skip_reason;
};

CurrentTest& Current() {
  static CurrentTest current;
  return current;
}

}  // namespace

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

Registrar::Registrar(const char* name, TestFunction function) {
  Registry().push_back(TestCase{name, function});
}

void ReportFailure(const char* file, int line, const std::string& message) {
  ++Current().failure_count;
  std::cerr << "  " << file << ":" << line << ": " << message << "\n";
}

void MarkSkipped(const std::string& reason) {
  Current().is_skipped = true;
  Current().skip_reason = reason;
}

}  // namespace strata::testing

// Usage: strata_tests [name-substring]. Runs the tests whose name contains
// the substring, or every test when it is omitted.
int main(int argc, char** argv) {
  using strata::testing::Registry;
  const char* filter = argc > 1 ? argv[1] : "";
  int passed = 0;
  int failed = 0;
  int skipped = 0;
  for (const auto& test_case : Registry()) {
    if (std::strstr(test_case.name, filter) == nullptr) {
      continue;
    }
    strata::testing::Current() = strata::testing::CurrentTest();
    std::cerr << "[ RUN  ] " << test_case.name << "\n";
    test_case.function();
    const auto& current = strata::testing::Current();
    if (current.is_skipped) {
      ++skipped;
      std::cerr << "[ SKIP ] " << test_case.name << ": " << current.skip_reason
                << "\n";
    } else if (current.failure_count > 0) {
      ++failed;
      std::cerr << "[ FAIL ] " << test_case.name << "\n";
    } else {
      ++passed;
      std::cerr << "[  OK  ] " << test_case.name << "\n";
    }
  }
  std::cerr << "passed=" << passed << " failed=" << failed
            << " skipped=" << skipped << "\n";
  return failed == 0 ? 0 : 1;
}
