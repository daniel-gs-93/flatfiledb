#include "strata/paged/crc32c.h"

#include <array>

namespace strata::paged {

namespace {

// The Castagnoli polynomial 0x1EDC6F41 in reflected form.
constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> BuildTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t byte_value = 0; byte_value < 256; ++byte_value) {
    std::uint32_t remainder = byte_value;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 1u) != 0
                      ? (remainder >> 1) ^ kReflectedPolynomial
                      : remainder >> 1;
    }
    table[byte_value] = remainder;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = BuildTable();

}  // namespace

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace strata::paged
