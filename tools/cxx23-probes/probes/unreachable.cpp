#include <utility>
int f(int x) { switch (x) { case 0: return 1; default: std::unreachable(); } }
int main() { return f(0) == 1 ? 0 : 1; }
