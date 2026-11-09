#define emit(x) do { (void)(x); } while (0);
#define guard(x) if ((x) < 0) return 0;
#define finish(x) return (x);

int before() { return 1; }
int run(int value) {
    emit((value + 1)) /* a physical boundary, not an expansion */
    guard(value)
    int saved = value;
    finish(saved)
}
int after() { return 2; }
