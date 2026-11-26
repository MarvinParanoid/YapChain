#include <array>
#include <ranges>
int main() {
    std::array a{1, 2, 3, 4, 5}; int n = 0;
    for (auto c : a | std::views::chunk(2)) n += static_cast<int>(std::ranges::distance(c));
    for (auto w : a | std::views::slide(2)) n += static_cast<int>(std::ranges::distance(w));
    for (int x : a | std::views::stride(2)) n += x;
    return n == 5 + 8 + 9 ? 0 : 1;
}
