#define offsetof(type, member) __builtin_offsetof(type, member)
#define LIB_BEGIN
#define LIB_END
struct Inner {
    int value;
};
struct Outer {
    int head;
    Inner inner[2];
};
struct Table {
    enum : unsigned { Empty = 0 };
    int count;
};
int inner_offset() {
    return offsetof(Outer, inner[1].value);
}
LIB_BEGIN
inline namespace v1 {
int version() {
    return 1;
}
}
LIB_END
