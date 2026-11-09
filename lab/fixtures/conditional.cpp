#define ENABLED 1
#if ENABLED
#define API_CALL __cdecl
#else
#define API_CALL
#endif
int API_CALL enabled() { return 1; }
#undef API_CALL
int compute(int a, int API_CALL, int c) { a * API_CALL * c; return 0; }
