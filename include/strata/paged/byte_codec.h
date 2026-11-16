#ifndef STRATA_PAGED_BYTE_CODEC_H_
#define STRATA_PAGED_BYTE_CODEC_H_

#include <cstdint>

// Fixed-width little-endian integers, written and read one byte at a time so
// the file does not depend on the host byte order or on struct padding
// (design document 2.10). Pure. The caller guarantees that the pointer has at
// least as many bytes as the integer is wide.

namespace strata::paged {

inline void WriteLe16(std::uint8_t* destination, std::uint16_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFFu);
  destination[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

inline void WriteLe32(std::uint8_t* destination, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    destination[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

inline void WriteLe64(std::uint8_t* destination, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    destination[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

inline std::uint16_t ReadLe16(const std::uint8_t* source) {
  return static_cast<std::uint16_t>(source[0] | (source[1] << 8));
}

inline std::uint32_t ReadLe32(const std::uint8_t* source) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(source[i]) << (8 * i);
  }
  return value;
}

inline std::uint64_t ReadLe64(const std::uint8_t* source) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(source[i]) << (8 * i);
  }
  return value;
}

}  // namespace strata::paged

#endif  // STRATA_PAGED_BYTE_CODEC_H_
