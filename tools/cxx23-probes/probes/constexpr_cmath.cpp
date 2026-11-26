#include <cmath>
static_assert(std::fabs(-2.0) == 2.0);  // P0533 (partial in libstdc++)
static_assert(std::fmax(1.0, 3.0) == 3.0);
int main() { return 0; }
