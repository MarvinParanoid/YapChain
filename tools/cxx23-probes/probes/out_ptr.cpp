#include <memory>
void make(int** p) { *p = new int(7); }
int main() { std::unique_ptr<int> u; make(std::out_ptr(u)); return *u == 7 ? 0 : 1; }
