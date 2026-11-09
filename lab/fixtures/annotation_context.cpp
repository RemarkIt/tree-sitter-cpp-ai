#define short_abi __cdecl
#define indirect_abi short_abi
int indirect_abi first() { return 1; }
#undef short_abi
#define short_abi __stdcall
int short_abi second() { return 2; }
#undef indirect_abi
#undef short_abi
int third(int a, int short_abi, int c) {
    a * short_abi * c;
    return 3;
}
