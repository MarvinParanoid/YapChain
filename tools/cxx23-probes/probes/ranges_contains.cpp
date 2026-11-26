#include <algorithm>
#include <array>
int main() { std::array a{1, 2, 3}; return std::ranges::contains(a, 2) ? 0 : 1; }
