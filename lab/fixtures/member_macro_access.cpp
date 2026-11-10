struct Event { int raise; };
#define WATCH_PROPERTY(type, name, notify) static_assert(sizeof(type) > 0, #name)
struct Model {
    Event Changed;
    WATCH_PROPERTY(int, Count, Changed.raise);
    int count() const { return 1; }
};
