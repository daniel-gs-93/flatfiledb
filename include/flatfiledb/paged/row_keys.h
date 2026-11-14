#ifndef FLATFILEDB_PAGED_ROW_KEYS_H_
#define FLATFILEDB_PAGED_ROW_KEYS_H_

#include <string>

#include "flatfiledb/domain/value.h"

// Hash keys for values (design document 2.3, "Hash key"). Pure: no I/O, no
// state. A key is a byte string used inside one process (hash tables of the
// operators and of the mutation planner); it is never stored, so its byte
// layout is not a contract. The flatfile executor has equivalent functions
// that are kept separate on purpose (ADR-0039).

namespace flatfiledb::paged {

// Appends the exact encoding of value to *key. Two values give the same bytes
// if and only if they have the same type and the same content, where -0.0 and
// 0.0 are the same content. The encoding is self-delimiting, so appending the
// encodings of several values one after another cannot collide: ('ab', 'c'
// and 'a', 'bc' give different bytes). Precondition: key is not null.
void AppendExactKey(const domain::Value& value, std::string* key);

// The exact encoding of one value; see AppendExactKey.
std::string ExactKey(const domain::Value& value);

// The join encoding of one value: INT and DOUBLE share one key space through
// their double value, so 2 and 2.0 give the same bytes, and a TEXT never
// equals a number. Two INT values above 2^53 can share a double, so a caller
// that finds a bucket by JoinKey must confirm the hit with domain::ValuesEqual.
// Not self-delimiting: it is the key of a single column.
std::string JoinKey(const domain::Value& value);

}  // namespace flatfiledb::paged

#endif  // FLATFILEDB_PAGED_ROW_KEYS_H_
