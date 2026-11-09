#define finish_ptr( ) return &base;
#define finish_ref( ) return base;
#define finish_value( ) return base;
#define finish_member( ) return &base;
int base = 0;
int *pointer() { finish_ptr()}
int &reference() { finish_ref()}
int (parenthesized)() { finish_value()}
struct Holder {
    int *method() { finish_member()}
};
