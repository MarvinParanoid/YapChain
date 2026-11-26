#include <mdspan>
int main() { int d[6] = {0, 1, 2, 3, 4, 5}; std::mdspan m(d, 2, 3); return m[1, 2] == 5 ? 0 : 1; }
