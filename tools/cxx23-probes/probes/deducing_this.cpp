struct S { int v = 3; int get(this const S& self) { return self.v; } };
int main() { S s; return s.get() == 3 ? 0 : 1; }
