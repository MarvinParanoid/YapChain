#include <memory>
constexpr int f() { auto p = std::make_unique<int>(5); return *p; }  // P2273
static_assert(f() == 5);
int main() { return 0; }
