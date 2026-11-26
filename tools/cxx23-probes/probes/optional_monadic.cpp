#include <optional>
int main() {
    std::optional<int> o = 4;
    auto r = o.and_then([](int x) -> std::optional<int> { return x * 2; }).transform([](int x) { return x + 1; }).or_else([] { return std::optional<int>(0); });
    return *r == 9 ? 0 : 1;
}
