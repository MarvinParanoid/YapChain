int f(int x) { [[assume(x > 0)]]; return x / 2; }
int main() { return f(4) == 2 ? 0 : 1; }
