#define CALLBACK_NOEXCEPT noexcept
#define CALL_CONVENTION
int apply(int (*callback)(int), int value) {
    return callback(value);
}
int run(int seed) {
    auto twice = [](int x) CALLBACK_NOEXCEPT { return x * 2; };
    int total = apply([](int x) CALL_CONVENTION { return x + 1; }, seed);
    return twice(total);
}
int after(int x) {
    return x;
}
