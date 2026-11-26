#include <flat_map>
int main() { std::flat_map<int, int> m; m[2] = 1; m[1] = 0; return m.begin()->first == 1 ? 0 : 1; }
