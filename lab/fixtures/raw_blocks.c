#define region(x) if (x)

int run(int value) {
    region(value)
    {
        int saved = value;
        value += saved;
    }
    return value;
}
int after(void) { return 2; }
