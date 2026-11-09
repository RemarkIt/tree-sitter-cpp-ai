#define emit(x) (void)(x);
#define finish(x) return (x);

int run(int value) {
    emit(value) // trailing comment
    int saved = value;
    finish(saved)}
int after(void) { return 2; }
