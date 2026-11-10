namespace json {
struct parse_error {};
int parse(const char *text);
}
#define CHECK_THROWS_AS(expr, type) ((void)sizeof(expr), (void)sizeof(type))
#define CHECK_THROWS_WITH_AS(expr, what, type) CHECK_THROWS_AS(expr, type)
void check_parse() {
    CHECK_THROWS_AS(json::parse("x"), json::parse_error&);
    int after_parse = 1;
}
void check_message() {
    CHECK_THROWS_WITH_AS(json::parse("y"), "bad input", json::parse_error*);
    int after_message = 2;
}
