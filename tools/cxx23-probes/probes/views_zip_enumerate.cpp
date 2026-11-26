#include <array>
#include <ranges>
int main() {
    std::array a{1, 2}, b{3, 4}; int s = 0;
    for (auto [x, y] : std::views::zip(a, b)) s += x * y;
    for (auto [i, x] : std::views::enumerate(a)) s += static_cast<int>(i) * x;
    return s == 13 ? 0 : 1;
}
