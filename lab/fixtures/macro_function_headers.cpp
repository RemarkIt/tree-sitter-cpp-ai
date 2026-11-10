#define METHOD_(type, name) virtual type name
#define FN(type, name) inline type name
#define NOEXCEPT noexcept

struct Iface {
    METHOD_(void, Run)() = 0;
    METHOD_(int, Count)() const = 0;
};

struct Impl : Iface {
    METHOD_(void, Run)() override {}
    METHOD_(int, Count)() const override;
};

namespace detail {
FN(bool, Check)(int value) NOEXCEPT;
FN(int, Twice)(int value) NOEXCEPT { return value * 2; }
}

FN(bool, Check)(int input) NOEXCEPT;
FN(int, Twice)(int input) NOEXCEPT { return input * 2; }

int after(int x) { return x; }
