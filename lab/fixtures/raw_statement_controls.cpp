int call(int);
using Callback = int (*)(int);
Callback factory(int);
int run(int a, int b) {
    call(a)
        /* still an ordinary call */ ;
    factory(a)
        (b);
    call(a)
        * b;
    call(a)
        and b;
    call(a)
        not_eq b;
    call(a) \
        + b;
    const char *label = "emit(a)\n{not code}";
    const char *raw = R"tag(emit(a)
/* not code */)tag";
    return label[0] + raw[0];
}
