#define VIS inline
#define NOEXCEPT noexcept
struct Pair {
    int a;
    int b;
    VIS constexpr Pair() NOEXCEPT : a(0), b(0) {}
    VIS explicit Pair(int v) noexcept : a(v), b(v) {}
};
int after(int x) {
    return x;
}
