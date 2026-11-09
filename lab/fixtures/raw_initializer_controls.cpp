using Number = int;
int get();
int run() {
    Number first{get()};
    Number second{
        get()
    };
    return first + second;
}
