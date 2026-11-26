#include <functional>
#include <memory>
int main() {
    auto p = std::make_unique<int>(41);
    std::move_only_function<int()> f = [q = std::move(p)] { return *q + 1; };
    return f() == 42 ? 0 : 1;
}
