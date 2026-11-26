#include <expected>
#include <string>
std::expected<int, std::string> half(int x) { if (x % 2) return std::unexpected("odd"); return x / 2; }
int main() {
    auto r = half(8).and_then(half).transform([](int v) { return v + 1; });
    return (r.has_value() && *r == 3 && !half(3) && half(3).error() == "odd") ? 0 : 1;
}
