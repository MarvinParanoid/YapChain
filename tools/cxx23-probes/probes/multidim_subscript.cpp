struct M { int d[4]{}; int& operator[](int r, int c) { return d[r * 2 + c]; } };
int main() { M m; m[1, 1] = 9; return m.d[3] == 9 ? 0 : 1; }
