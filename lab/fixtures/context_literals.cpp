#define local_abi __cdecl
// local_abi is not an annotation inside a comment.
const char *label = "local_abi";
const char *raw_label = R"tag(
local_abi " // /* not code
)tag";
int local_abi run() { return 1; }
