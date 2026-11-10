#define NOINLINE_API __attribute__((noinline))
#define VISIBLE __attribute__((visibility("default")))
#define IN_PARAM
struct Provider {
    int id;
    VISIBLE virtual ~Provider() {}
    void move(IN_PARAM Provider const& other, int x) { id = x; }
    NOINLINE_API VISIBLE
    static int count(int value) { return value; }
};
