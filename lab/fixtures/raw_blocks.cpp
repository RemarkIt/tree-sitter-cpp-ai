#define region(x) if (x)

int before() { return 1; }
int run(int value) {
    region((value + 1)) /* not a generated function */ {
        int saved = value;
        value += saved;
    }
    return value;
}
int after() { return 2; }
