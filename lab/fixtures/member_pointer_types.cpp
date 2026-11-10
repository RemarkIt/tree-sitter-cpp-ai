template <class T> struct Traits {};
template <class R, class C> struct Traits<R C::*> {};
template <class R, class C, class... A> struct Traits<R (C::*)(A...)> {};
template <class R, class C> struct Traits<R (C::*)(...) const volatile> {};
template <class R, class C, class... A> struct Traits<R (C::*)(A...) &&> {};
struct Box {
    int value;
};
using Method = void (Box::*)(int) const;
using Field = int Box::*;
char probe(int Box::*);
int read(const Box& b, int Box::* field) { return b.*field; }
int after(int x) {
    return x;
}
