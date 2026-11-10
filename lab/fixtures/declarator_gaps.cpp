template <bool B, typename T = void> struct enable_if {};
template <typename T> struct enable_if<true, T> { typedef T type; };
template <typename T> struct Box {};
template struct Box<int>;
struct Pool {
    static void __stdcall callback(void* context) {}
    int width;
};
template <typename T, typename enable_if<sizeof(T) == 4>::type* = nullptr>
int sized(T value) { return 4; }
struct Number {
    operator int() const { return 7; }
};
int read(Number number) {
    int value = number.operator int();
    return value;
}
int** grid() {
    return new int*[3];
}
