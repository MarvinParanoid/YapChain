#include <cstddef>
#include <type_traits>
static_assert(std::is_same_v<decltype(5uz), std::size_t>);
int main() { return 0; }
