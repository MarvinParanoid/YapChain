constexpr int f() { if consteval { return 1; } else { return 2; } }
static_assert(f() == 1);
int main() { return f() == 2 ? 0 : 1; }
