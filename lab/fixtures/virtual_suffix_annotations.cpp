#define FORMAT_CHECK(a, b)
#define NO_ANALYSIS
struct Base {
    virtual void log(const char* text) {}
    virtual int level() { return 0; }
};
struct Logger : Base {
    void log(const char* text) override FORMAT_CHECK(1, 2) {}
    int level() final NO_ANALYSIS { return 1; }
};
int after(int x) {
    return x;
}
