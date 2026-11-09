int get();
namespace numbers { using Integer = int; }
template<class T> using Single = T;
int scalar{get()};
numbers::Integer qualified{get()};
Single<int> templated{
    get()
};
int array[2]{get(), get()};
int nested() {
    int local{[]() { return get(); }()};
    return local;
}
struct Holder {
    int member{get()};
    int method() { return member; }
};
