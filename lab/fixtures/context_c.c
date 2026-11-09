#define short_abi __cdecl
int short_abi add(int a, int b) { return a + b; }
#undef short_abi
int product(int a, int short_abi, int c) {
    a * short_abi * c;
    return 0;
}
