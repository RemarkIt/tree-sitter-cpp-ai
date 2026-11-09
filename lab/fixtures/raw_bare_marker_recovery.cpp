#define trace() do {} while (0);
#define sentinel
namespace types { using Number = int; }
int qualified() {
    trace()
    sentinel
    types::Number slot{1};
    return slot;
}
int block() {
    trace()
    sentinel {
        int saved = 1;
        return saved;
    }
}
int after() { return 0; }
