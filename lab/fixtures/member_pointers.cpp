struct Benchmark {
    void run(int n) {}
    int count;
};
struct Runner {
    void (Benchmark::*method)(int);
    Benchmark* bench;
    int Benchmark::*counter;
    void start(int n) { (bench->*method)(n); }
    int total() const { return bench->*counter; }
};
