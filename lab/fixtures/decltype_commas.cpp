struct Text {
    int* data() const { return nullptr; }
    int size() const { return 0; }
};
template <class U> auto size_of(U value) -> decltype(value.data(), value.size()) {
    return value.size();
}
int measure(Text text) {
    decltype(text.data(), text.size()) length = text.size();
    return length;
}
