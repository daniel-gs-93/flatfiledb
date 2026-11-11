#ifndef FLATFILEDB_PAGED_CRC32C_H_
#define FLATFILEDB_PAGED_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace flatfiledb::paged {

// CRC-32C (Castagnoli polynomial, reflected, initial value and final XOR
// 0xFFFFFFFF) of size bytes; the CRC of "123456789" is 0xE3069283. Pure.
// Cost: O(size), one table lookup per byte.
std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size);

}  // namespace flatfiledb::paged

#endif  // FLATFILEDB_PAGED_CRC32C_H_
