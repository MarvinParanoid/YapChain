#include <algorithm>
#include <array>
int main() {
    std::array a{1, 2, 3};
    std::array b{1, 2};
    return (std::ranges::starts_with(a, b) && std::ranges::ends_with(a, std::array{2, 3})) ? 0 : 1;
}
