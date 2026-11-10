struct Box {
    int size;
    Box();
    Box(const Box& other) = delete;
    template <class U> void take(U value) = delete;
};
Box::Box() = default;
template <class T> void release(T* item) = delete;
void release(int count) = delete;
int after(int x) {
    return x;
}
