struct Base {};
Base make();
struct Derived : decltype(make()) { int extra; };
struct D2 : public decltype((make())) { int more; };
int capture(int v) { auto f = [w{ v }, v](int a) { return a + w + v; }; return f(1); }
struct Handle { operator int*() const; };
int* raw(const Handle& h) { return h.operator int*(); }
template <class T> int make_tag() { auto t = typename T::type{}; return 0; }
int after(int x) { return x; }
