// Links only with -lstdc++exp in libstdc++ 13/14 (still experimental there): counted as unavailable.
#include <stacktrace>
int main() { auto st = std::stacktrace::current(); return st.size() >= 0 ? 0 : 1; }
