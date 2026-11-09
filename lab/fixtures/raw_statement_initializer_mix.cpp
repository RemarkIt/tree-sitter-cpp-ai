#define observe(x) do { (void)(x); } while (0);
int get();
int run(int value) {
    observe(value)
    int scalar{get()};
    int nested{[&]() {
        observe(value + 1)
        return get();
    }()};
    if (value) {
        observe(value + 2)} else {
        observe(0)}
    return scalar + nested;
}
int after() { return 0; }
