#include <generator>
std::generator<int> g() { for (int i = 0; i < 3; ++i) co_yield i; }
int main() { int s = 0; for (int x : g()) s += x; return s == 3 ? 0 : 1; }
