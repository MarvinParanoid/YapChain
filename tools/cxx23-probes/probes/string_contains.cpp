#include <string>
#include <string_view>
static_assert(std::string_view("hello world").contains("o w"));
static_assert(std::string_view("abc").contains('b'));
int main() { std::string s = "yapchain"; return (s.contains("chain") && !s.contains('z')) ? 0 : 1; }
