void release(int handle);
#define SCOPE_EXIT [&]()
int bind(int handle) {
    SCOPE_EXIT {
        release(handle);
    };
    int bound = handle + 1;
    return bound;
}
