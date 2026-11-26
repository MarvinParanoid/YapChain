#include <ranges>
#include <vector>
int main() {
    auto v = std::views::iota(0, 5) | std::views::transform([](int i) { return i * i; }) | std::ranges::to<std::vector>();
    return (v.size() == 5 && v[4] == 16) ? 0 : 1;
}
