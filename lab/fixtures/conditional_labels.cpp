#define HAS_FEATURE(token) 1
#define TRACE_EXIT 1
#if HAS_FEATURE(?)
int feature_level = 1;
#endif
int run(int value) {
    if (value < 0)
        goto done;
    value++;
#if defined(TRACE_EXIT)
done:
#endif
    return value;
}
void finish(int value) {
    if (value)
        goto end;
    value = 0;
end:
}
