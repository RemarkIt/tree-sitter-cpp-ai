#include "context_abi.h"
int local_abi declaration() { return 1; }
#undef local_abi
int product(int a, int local_abi, int c) {
    a * local_abi * c;
    return 0;
}
