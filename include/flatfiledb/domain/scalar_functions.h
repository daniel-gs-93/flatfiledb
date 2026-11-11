#ifndef FLATFILEDB_DOMAIN_SCALAR_FUNCTIONS_H_
#define FLATFILEDB_DOMAIN_SCALAR_FUNCTIONS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/plan.h"
#include "flatfiledb/domain/value.h"

// The pure helpers behind the scalar functions and LIKE of
// docs/DesignDocument.md section 2.3.

namespace flatfiledb::domain {

// LIKE: '%' matches any run of bytes (including none), '_' matches one UTF-8
// character (an invalid byte counts as one character), every other pattern
// byte matches itself with ASCII letters compared case-insensitively. Runs in
// O(text.size() * pattern.size()) time and constant space.
bool LikeMatches(const std::string& text, const std::string& pattern);

// The integer in text ([+-]?[0-9]+ within int64_t) or std::nullopt; the same
// rule as CAST(text AS INT).
std::optional<int64_t> ParseIntegerText(const std::string& text);

// The finite decimal in text ([+-]?[0-9]+(\.[0-9]+)?) or std::nullopt; the same
// rule as CAST(text AS DOUBLE).
std::optional<double> ParseDecimalText(const std::string& text);

// Applies a scalar function to arguments that the Binder has already checked
// for count and type. result_type is the type of the call, which for kCast is
// the target type. Returns std::nullopt on a runtime failure and, when failure
// is not null, writes the kRuntime message there (for example
// "negative substring length"); *failure is not written on success.
std::optional<Value> ApplyScalarFunction(ScalarFn fn, ExprType result_type,
                                         const std::vector<Value>& arguments,
                                         std::string* failure);

}  // namespace flatfiledb::domain

#endif  // FLATFILEDB_DOMAIN_SCALAR_FUNCTIONS_H_
