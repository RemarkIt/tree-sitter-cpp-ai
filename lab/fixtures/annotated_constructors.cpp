#define FORCE_INLINE inline
#define NO_DEBUG_STEP inline
struct Vec {
    float x;
    float y;
    NO_DEBUG_STEP constexpr Vec() : x(0.0f), y(0.0f) {}
    NO_DEBUG_STEP constexpr Vec(float a, float b) : x(a), y(b) {}
};
struct Holder {
    int* value;
    FORCE_INLINE Holder(int* p) :
            value(p) {}
    FORCE_INLINE explicit Holder(int n) : value(nullptr) {}
};
int after(int x) {
    return x;
}
