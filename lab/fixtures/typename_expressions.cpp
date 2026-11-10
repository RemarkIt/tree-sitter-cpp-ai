template <class T> struct Wrapper { typedef T type; };
#define CHECK_TYPE(expr, type) ((void)sizeof(expr), (void)sizeof(type))
template <class T> int convert(T value) {
    T copy = typename Wrapper<T>::type(value);
    CHECK_TYPE(value, typename Wrapper<T>::type&);
    return 1;
}
