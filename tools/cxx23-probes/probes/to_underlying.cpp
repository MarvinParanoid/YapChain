#include <cstdint>
#include <utility>
enum class E : std::uint8_t { a = 7 };
static_assert(std::to_underlying(E::a) == 7);
static_assert(std::is_same_v<decltype(std::to_underlying(E::a)), std::uint8_t>);
static_assert(std::is_scoped_enum_v<E>);
int main() { return 0; }
