#include <vector>
int main() { std::vector<int> v{1, 2}; auto w = auto(v); w.push_back(3); return (v.size() == 2 && w.size() == 3) ? 0 : 1; }
