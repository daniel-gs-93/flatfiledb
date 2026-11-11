#ifndef FLATFILEDB_TESTS_TEST_HARNESS_H_
#define FLATFILEDB_TESTS_TEST_HARNESS_H_

#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Minimal test harness. A test is a void function registered with
// TEST; CHECK records a failure and continues, SKIP ends the test as skipped.

namespace flatfiledb::testing {

using TestFunction = void (*)();

struct TestCase {
  const char* name;
  TestFunction function;
};

std::vector<TestCase>& Registry();

class Registrar {
 public:
  Registrar(const char* name, TestFunction function);
};

// Both act on the test that is currently running.
void ReportFailure(const char* file, int line, const std::string& message);
void MarkSkipped(const std::string& reason);

// Lets CHECK_EQ print vectors, which the standard library does not stream.
template <typename T>
std::ostream& operator<<(std::ostream& stream, const std::vector<T>& values) {
  stream << "{";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      stream << ", ";
    }
    stream << values[i];
  }
  return stream << "}";
}

template <typename Left, typename Right>
std::string DescribeMismatch(const char* left_expr, const char* right_expr,
                             const Left& left, const Right& right) {
  std::ostringstream stream;
  stream << left_expr << " == " << right_expr << " failed: [" << left
         << "] vs [" << right << "]";
  return stream.str();
}

}  // namespace flatfiledb::testing

#define TEST(name)                                                        \
  static void name();                                                     \
  static ::flatfiledb::testing::Registrar registrar_##name(#name, &name); \
  static void name()

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      ::flatfiledb::testing::ReportFailure(__FILE__, __LINE__,              \
                                           "CHECK(" #condition ") failed"); \
    }                                                                       \
  } while (false)

#define CHECK_EQ(left, right)                                       \
  do {                                                              \
    auto&& check_left_value = (left);                               \
    auto&& check_right_value = (right);                             \
    if (!(check_left_value == check_right_value)) {                 \
      ::flatfiledb::testing::ReportFailure(                         \
          __FILE__, __LINE__,                                       \
          ::flatfiledb::testing::DescribeMismatch(                  \
              #left, #right, check_left_value, check_right_value)); \
    }                                                               \
  } while (false)

#define SKIP(reason)                            \
  do {                                          \
    ::flatfiledb::testing::MarkSkipped(reason); \
    return;                                     \
  } while (false)

#endif  // FLATFILEDB_TESTS_TEST_HARNESS_H_
