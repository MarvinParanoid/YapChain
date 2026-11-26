#include <span>
#include <spanstream>
int main() { char buf[16]{}; std::ospanstream os{std::span<char>(buf)}; os << 42; return buf[0] == '4' ? 0 : 1; }
