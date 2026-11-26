#include <bitset>
constexpr bool f() { std::bitset<8> b; b.set(3); b.flip(0); return b.count() == 2 && b.test(3); }  // P2417
static_assert(f());
int main() { return 0; }
