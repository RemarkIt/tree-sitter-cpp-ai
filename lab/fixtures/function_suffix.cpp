#define NOEXCEPT_MACRO noexcept
#define LOCKS_EXCLUDED(lock) __attribute__((noinline))
#define FORMAT_LIKE(index, first) __attribute__((format(printf, index, first)))
#define LIFETIME_BOUND __attribute__((pure))
#define BLOCKING __attribute__((noinline))
struct Mutex {};
class Holder {
public:
    explicit Holder(Mutex* mu) LOCKS_EXCLUDED(mu) : mu_(mu) {}
    int size() const NOEXCEPT_MACRO { return 0; }
    int& at(int index) const LIFETIME_BOUND;
    virtual void log(const char* format, ...) FORMAT_LIKE(2, 3) = 0;
    void wait(int count)
        BLOCKING;
private:
    Mutex* mu_;
};
int compute(int value) BLOCKING;
int compute(int value) NOEXCEPT_MACRO
{
    int doubled = value * 2;
    return doubled;
}
