struct Box {
    int size;
};
int measure(Box box) {
    return box.size;
}
int copy_size(Box box) {
    int copied = measure(decltype(box)(box));
    auto empty = decltype(box){};
    decltype(box)(other);
    return copied + empty.size + other.size;
}
