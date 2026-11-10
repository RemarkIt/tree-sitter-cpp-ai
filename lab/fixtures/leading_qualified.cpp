#define FORCE_INLINE inline
#define NOINLINE __attribute__((noinline))
#define EXPORT_API __attribute__((visibility("default")))
#define IN_PARAM
namespace util {
struct Point;
template <typename T> struct Box;
}
FORCE_INLINE util::Point &pick(util::Point &point) { return point; }
NOINLINE util::Box<int> *
unwrap(util::Box<int> *box) { return box; }
EXPORT_API static util::Point *find(util::Point *points) { return points; }
int count(IN_PARAM const util::Point &first, IN_PARAM util::Box<int> *box) { return 2; }
