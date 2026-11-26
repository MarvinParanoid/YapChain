#include <utility>
#include <type_traits>
struct S { int m; };
static_assert(std::is_same_v<decltype(std::forward_like<S&&>(std::declval<int&>())), int&&>);
int main() { return 0; }
