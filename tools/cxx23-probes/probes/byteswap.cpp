#include <bit>
#include <cstdint>
static_assert(std::byteswap(std::uint32_t{0x01020304}) == 0x04030201u);
static_assert(std::byteswap(std::uint64_t{0x0102030405060708}) == 0x0807060504030201ull);
int main() { volatile std::uint16_t v = 0x1234; return std::byteswap(static_cast<std::uint16_t>(v)) == 0x3412 ? 0 : 1; }
