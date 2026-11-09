#define WIDE_TEXT(text) L##text
#define WIDE(text) WIDE_TEXT(text)
#define VERSION_TEXT "1.2"
#define STRINGIFY_TEXT(value) #value
#define STRINGIFY(value) STRINGIFY_TEXT(value)
#define VERSION_NUMBER 12
#define PICK_A(value)
#define PICK_B(value) value
const wchar_t *message = WIDE("first ")
                         WIDE("second") L"!";
const char *version = "Version " VERSION_TEXT " (" STRINGIFY(VERSION_NUMBER) ")";
int choose() {
    return PICK_A(1) PICK_B(2);
}
typedef const wchar_t *WideText;
WideText label() {
    return WIDE("V ")WIDE(VERSION_TEXT);
}
