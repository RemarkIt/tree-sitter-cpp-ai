#define scenario(label) int generated()
scenario("a label, not a declaration") { return 1; }
int after() { return 2; }
