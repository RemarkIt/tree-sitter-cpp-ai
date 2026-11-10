int next(int value) {
    return value + 1;
}
int scan(int limit) {
    int a = 0;
    int b = 0;
    int* p = (a = next(limit), &a);
    while ((b = next(b), b < limit)) {
    }
    return *p + b;
}
int after(int x) {
    return x;
}
