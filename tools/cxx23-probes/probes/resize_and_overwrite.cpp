#include <cstring>
#include <string>
int main() { std::string s; s.resize_and_overwrite(8, [](char* p, std::size_t) { std::memcpy(p, "yap", 3); return std::size_t{3}; }); return s == "yap" ? 0 : 1; }
