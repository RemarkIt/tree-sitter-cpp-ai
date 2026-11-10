#define _Post_satisfies_(condition) __attribute__((warn_unused_result))
#define _In_reads_bytes_(size)
#define _When_(condition, annotation)
_Post_satisfies_(return == true) inline bool verify(int value) { return value == 1; }
int copy(_In_reads_bytes_(size) const char* data, unsigned size);
int debug(_When_(text != nullptr, size > 0) unsigned size, const char* text);
