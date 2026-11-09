using Fn = int (*)(int);
Fn choose();
int (*callback)(int){choose()};
int (*callbacks[2])(int){choose(), choose()};
int base = 0;
int *pointer() { return &base; }
int &reference() { return base; }
int (parenthesized)() { return base; }
struct Holder {
    int (*member)(int){choose()};
    int *method() { return &base; }
};
