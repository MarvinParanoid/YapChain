struct Less { static bool operator()(int a, int b) { return a < b; } };
int main() { return Less{}(1, 2) ? 0 : 1; }
