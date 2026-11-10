#define NOEXCEPT_MACRO noexcept
#define NORETURN __attribute__((noreturn))
#define VISIBLE __attribute__((visibility("default")))
struct Info {
    int id;
    Info(Info const& other) NOEXCEPT_MACRO
    {
        id = other.id;
    }
};
NORETURN inline VISIBLE void stop() { __builtin_trap(); }
