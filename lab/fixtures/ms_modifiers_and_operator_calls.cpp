struct __declspec(uuid("a817e7a2-43fa-11d0-9e44-00aa00b6770a")) Fake {
    int count;
};
inline void* __stdcall capture() { return 0; }
struct Resource {
    int value;
    void assign(const Resource& other) {
        operator=(other);
    }
    static void* __stdcall allocate(int size) { return 0; }
};
