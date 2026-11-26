#include <algorithm>
#include <array>
int main() { std::array a{1, 2, 3}; return std::ranges::fold_left(a, 0, [](int x, int y) { return x + y; }) == 6 ? 0 : 1; }
