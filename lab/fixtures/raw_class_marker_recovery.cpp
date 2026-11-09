#define expose(Kind)
using Token = int;
struct Holder {
    expose(Holder)
public:
    void configure(const Token& token);
};
