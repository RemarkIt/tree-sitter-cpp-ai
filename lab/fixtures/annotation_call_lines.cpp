#define ANNOT(x) inline
#define POST(x) inline
#define WHEN(a, b) static
#define IRQL(x) constexpr
#define HOTWORD
template <class T>
ANNOT(true)
int first(T value);
template <class T>
ANNOT(true)
int first_body(T value) { return 0; }
POST(x) WHEN(a, b) int second(int hr);
POST(x) WHEN(a, b)
int second_wrapped(int hr);
inline IRQL(2)
void third(int x) {}
template <class T>
ANNOT(true)
HOTWORD
__declspec(noinline) inline int fourth(int a) { return a; }
template <class T>
ANNOT(true)
int
    fifth(T value) { return 0; }
int after(int x) { return x; }
