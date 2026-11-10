#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <assert.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    RAW_STRING_DELIMITER,
    RAW_STRING_CONTENT,
    LEADING_ANNOTATION,
    DECLARATOR_ANNOTATION,
    EXPRESSION_START_PROBE,
    MACRO_INVOCATION_CALLEE,
    INITIALIZER_START_PROBE,
    MACRO_TYPE_CALLEE,
    PREPROC_INLINE_CONDITIONAL,
    PREPROC_INACTIVE_BRANCH,
    PREPROC_IF_PROBE,
    PREPROC_END_OF_INPUT,
    CLASS_ANNOTATION_CALLEE,
    CLASS_LEADING_ANNOTATION,
    FUNCTION_SUFFIX_ANNOTATION,
    JUXTAPOSED_CALLEE,
    PREPROC_RAW_ARGUMENT,
    SECTION_LABEL,
    IDENTIFIER,
    LITERAL_SUFFIX,
};

/// The spec limits delimiters to 16 chars
#define MAX_DELIMITER_LENGTH 16

typedef struct {
    uint8_t delimiter_length;
    wchar_t delimiter[MAX_DELIMITER_LENGTH];
    /// Conditionals taken in place whose `#endif` has not been consumed yet.
    uint8_t inline_conditionals;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void reset(Scanner *scanner) {
    scanner->delimiter_length = 0;
    memset(scanner->delimiter, 0, sizeof scanner->delimiter);
}

/// Words longer than any grammar keyword are always ordinary identifiers.
#define MAX_WORD_LENGTH 24

typedef struct {
    char text[MAX_WORD_LENGTH + 1];
    uint8_t length;
    bool truncated;
    bool non_ascii;
} Word;

// Classify ASCII inline and consult <wctype.h> only beyond ASCII, keeping the common case cheap.
static inline bool is_ascii_letter(int32_t c) { return (uint32_t)((c | 0x20) - 'a') < 26; }

static inline bool is_digit(int32_t c) { return (c >= '0' && c <= '9') || (c > 0x7f && iswdigit(c)); }

static inline bool is_space(int32_t c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c > 0x7f && iswspace(c)); }

static inline bool is_identifier_start(int32_t c) { return is_ascii_letter(c) || c == '_' || c == '$' || c > 0x7f; }

static inline bool is_identifier_part(int32_t c) {
    return is_ascii_letter(c) || (c >= '0' && c <= '9') || c == '_' || c == '$' || c > 0x7f;
}

static inline bool word_is(const Word *word, const char *text) {
    return !word->truncated && strcmp(word->text, text) == 0;
}

static bool read_word(TSLexer *lexer, Word *word) {
    word->length = 0;
    word->truncated = false;
    word->non_ascii = false;
    if (!is_identifier_start(lexer->lookahead)) {
        return false;
    }
    while (is_identifier_part(lexer->lookahead)) {
        if (word->length < MAX_WORD_LENGTH && lexer->lookahead < 0x80) {
            word->text[word->length++] = (char)lexer->lookahead;
        } else {
            word->truncated = true;
            word->non_ascii = word->non_ascii || lexer->lookahead > 0x7f;
        }
        advance(lexer);
    }
    word->text[word->length] = '\0';
    return true;
}

/// Binary search of a word list, which must be strictly ASCII-sorted (lab tests check this).
static bool word_in_sorted(const Word *word, const char *const *list, size_t count) {
    if (word->truncated) {
        return false;
    }
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        // Most probes differ in their first character, which decides them without a call.
        int order = (unsigned char)word->text[0] - (unsigned char)list[middle][0];
        if (order == 0) {
            order = strcmp(word->text, list[middle]);
        }
        if (order == 0) {
            return true;
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return false;
}

/// Word lists are NULL-terminated arrays, so their length is known at each call site.
#define word_in(word, list) word_in_sorted((word), (list), sizeof(list) / sizeof(*(list)) - 1)

/// Every identifier-shaped literal token of the grammar. Keywords are never annotations.
static const char *const KEYWORDS[] = {
    "FALSE", "NULL", "TRUE", "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Nonnull", "_Noreturn",
    "__alignof", "__alignof__", "__asm", "__asm__", "__attribute", "__attribute__", "__based", "__cdecl",
    "__clrcall", "__declspec", "__except", "__extension__", "__fastcall", "__finally", "__forceinline",
    "__inline", "__inline__", "__leave", "__restrict", "__restrict__", "__sptr", "__stdcall", "__thiscall",
    "__thread", "__try", "__unaligned", "__uptr", "__vectorcall", "__volatile__", "_alignof", "_unaligned",
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "char16_t", "char32_t", "char64_t", "char8_t", "charptr_t", "class", "co_await",
    "co_return", "co_yield", "compl", "concept", "const", "consteval", "constexpr", "constinit", "continue",
    "decltype", "default", "defined", "delete", "do", "double", "else", "enum", "explicit", "export",
    "extern", "false", "final", "float", "for", "friend", "goto", "if", "import", "inline", "int",
    "int16_t", "int32_t", "int64_t", "int8_t", "intptr_t", "long", "max_align_t", "module", "mutable",
    "namespace", "new", "noexcept", "noreturn", "not", "not_eq", "nullptr", "nullptr_t", "offsetof",
    "operator", "or", "or_eq", "override", "private", "protected", "ptrdiff_t", "public", "register",
    "requires", "restrict", "return", "short", "signed", "size_t", "sizeof", "ssize_t", "static",
    "static_assert", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try",
    "typedef", "typename", "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "union", "unsigned",
    "using", "virtual", "void", "volatile", "while", "xor", "xor_eq", NULL,
};

/// Keywords that can only begin a declared type. `auto` and `decltype(auto)` are excluded:
/// after an identifier they form a constrained placeholder such as `Integral auto`.
static const char *const TYPE_KEYWORDS[] = {
    "bool", "char", "char16_t", "char32_t", "char64_t", "char8_t", "charptr_t", "class", "double",
    "enum", "float", "int", "int16_t", "int32_t", "int64_t", "int8_t", "intptr_t",
    "long", "max_align_t", "nullptr_t", "ptrdiff_t", "short", "signed", "size_t", "ssize_t", "struct",
    "typename", "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "union", "unsigned", "void",
    NULL,
};

/// Declaration keywords that may separate leading annotations from the keyword type.
static const char *const SPECIFIER_KEYWORDS[] = {
    "_Noreturn", "__extension__", "__forceinline", "__inline", "__inline__", "__restrict__", "__thread",
    "const", "consteval", "constexpr", "constinit", "explicit", "extern", "friend", "inline", "mutable",
    "noreturn", "register", "restrict", "static", "thread_local", "virtual", "volatile", NULL,
};

/// Alternative operator spellings, which continue an expression after `)`.
static const char *const CONTINUATION_WORDS[] = {
    "and", "and_eq", "bitand", "bitor", "not_eq", "or", "or_eq", "xor", "xor_eq", NULL,
};

/// Words that continue a constructor declarator after its parameters. Constructors cannot be
/// `const`, `volatile`, `override` or `final`, and are only declared where no expression starts.
static const char *const DECLARATOR_SUFFIX_WORDS[] = {
    "__asm", "__asm__", "__attribute", "__attribute__", "asm", "noexcept", "requires", "throw", "try", NULL,
};

/// Keywords that end or continue a function declarator after an annotation following its
/// parameters, as in `f() A const`, `f() A noexcept` or `f() A override`.
static const char *const FUNCTION_SUFFIX_KEYWORDS[] = {
    "__asm", "__asm__", "__attribute", "__attribute__", "asm", "const", "final", "noexcept", "override",
    "requires", "throw", "try", "volatile", NULL,
};

/// Upper bound on bytes examined after a candidate callee, keeping scans linear in practice.
#define MAX_INVOCATION_SCAN 65536

static inline bool is_horizontal_space(int32_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

/// `auto`/`decltype` directly after an identifier form a constrained placeholder, but after a
/// specifier keyword they can only begin the declared type.
static inline bool is_placeholder_keyword(const Word *word) {
    return !word->truncated && (strcmp(word->text, "auto") == 0 || strcmp(word->text, "decltype") == 0);
}

static inline void skip_horizontal_space(TSLexer *lexer) {
    while (is_horizontal_space(lexer->lookahead)) {
        advance(lexer);
    }
}

/// After `A` (already consumed): the next token continues the declarator, i.e. another
/// identifier, `*` or `&`. A declarator name is never followed by these tokens.
static bool declarator_annotation_follows(TSLexer *lexer) {
    while (is_space(lexer->lookahead)) {
        advance(lexer);
    }
    if (lexer->lookahead == '*' || lexer->lookahead == '&') {
        return true;
    }
    Word word;
    if (!read_word(lexer, &word)) {
        return false;
    }
    return !word_in(&word, KEYWORDS) || strcmp(word.text, "operator") == 0;
}

/// Consume a comment after its leading `/` has been consumed. Returns false if this `/` is not
/// a comment. Sets `newline` when the comment ends a line or spans one.
static bool skip_comment_after_slash(TSLexer *lexer, bool *newline) {
    if (lexer->lookahead == '/') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
            if (lexer->lookahead == '\\') {
                advance(lexer);
                if (lexer->lookahead == '\r') {
                    advance(lexer);
                }
            }
            advance(lexer);
        }
        return true;
    }
    if (lexer->lookahead == '*') {
        advance(lexer);
        for (;;) {
            if (lexer->eof(lexer)) {
                return false;
            }
            if (lexer->lookahead == '\n') {
                *newline = true;
            }
            if (lexer->lookahead == '*') {
                advance(lexer);
                if (lexer->lookahead == '/') {
                    advance(lexer);
                    return true;
                }
                continue;
            }
            advance(lexer);
        }
    }
    return false;
}

/// Consume a quoted literal from its opening quote; false if it does not end on its line.
static bool skip_quoted(TSLexer *lexer) {
    int32_t quote = lexer->lookahead;
    advance(lexer);
    for (;;) {
        if (lexer->eof(lexer) || lexer->lookahead == '\n') {
            return false;
        }
        if (lexer->lookahead == '\\') {
            advance(lexer);
            if (lexer->eof(lexer)) {
                return false;
            }
            advance(lexer);
            continue;
        }
        if (lexer->lookahead == quote) {
            advance(lexer);
            return true;
        }
        advance(lexer);
    }
}

/// Consume a raw string literal from its opening quote.
static bool skip_raw_string(TSLexer *lexer) {
    int32_t delimiter[MAX_DELIMITER_LENGTH];
    int length = 0;
    advance(lexer);
    while (lexer->lookahead != '(') {
        if (length >= MAX_DELIMITER_LENGTH || lexer->eof(lexer) || lexer->lookahead == '\\' ||
            lexer->lookahead == ')' || is_space(lexer->lookahead)) {
            return false;
        }
        delimiter[length++] = lexer->lookahead;
        advance(lexer);
    }
    advance(lexer);
    for (;;) {
        if (lexer->eof(lexer)) {
            return false;
        }
        if (lexer->lookahead != ')') {
            advance(lexer);
            continue;
        }
        advance(lexer);
        int matched = 0;
        while (matched < length && lexer->lookahead == delimiter[matched]) {
            advance(lexer);
            matched++;
        }
        if (matched == length && lexer->lookahead == '"') {
            advance(lexer);
            return true;
        }
    }
}

static inline bool is_raw_prefix(const Word *word) {
    return !word->truncated && (strcmp(word->text, "R") == 0 || strcmp(word->text, "LR") == 0 ||
                                strcmp(word->text, "uR") == 0 || strcmp(word->text, "UR") == 0 ||
                                strcmp(word->text, "u8R") == 0);
}

/// From `(`, consume the balanced argument list through its matching `)`. Strings, raw
/// strings, character literals, numbers and comments cannot unbalance the scan. Records whether
/// an argument begins with a literal, which a parameter declaration never does.
static bool skip_balanced_arguments(TSLexer *lexer, bool *literal_argument) {
    unsigned depth = 0;
    bool argument_start = false;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            argument_start = c == '(' && depth == 1;
            advance(lexer);
        } else if (c == ')' || c == ']' || c == '}') {
            advance(lexer);
            if (--depth == 0) {
                return c == ')';
            }
            argument_start = false;
        } else if (is_space(c)) {
            advance(lexer);
        } else if (c == ',') {
            advance(lexer);
            argument_start = depth == 1;
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            if (!skip_comment_after_slash(lexer, &newline)) {
                argument_start = false;
            }
        } else if (c == '"' || c == '\'') {
            *literal_argument = *literal_argument || argument_start;
            if (!skip_quoted(lexer)) {
                return false;
            }
            argument_start = false;
        } else if (is_digit(c)) {
            *literal_argument = *literal_argument || argument_start;
            int32_t previous = 0;
            while (is_identifier_part(lexer->lookahead) || lexer->lookahead == '.' || lexer->lookahead == '\'' ||
                   ((lexer->lookahead == '+' || lexer->lookahead == '-') &&
                    (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P'))) {
                previous = lexer->lookahead;
                advance(lexer);
            }
            argument_start = false;
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (lexer->lookahead == '"' && is_raw_prefix(&word)) {
                if (!skip_raw_string(lexer)) {
                    return false;
                }
            } else if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                if (!skip_quoted(lexer)) {
                    return false;
                }
            }
            argument_start = false;
        } else if (c == '#') {
            return false;
        } else {
            advance(lexer);
            argument_start = false;
        }
    }
    return false;
}

/// Skip whitespace, comments and line continuations; false if a non-comment `/` is found.
static bool skip_trivia(TSLexer *lexer, bool *newline) {
    for (;;) {
        int32_t c = lexer->lookahead;
        if (c == '\n') {
            *newline = true;
            advance(lexer);
        } else if (is_space(c)) {
            advance(lexer);
        } else if (c == '/') {
            advance(lexer);
            if (!skip_comment_after_slash(lexer, newline)) {
                return false;
            }
        } else if (c == '\\') {
            advance(lexer);
            if (lexer->lookahead == '\r') {
                advance(lexer);
            }
            if (lexer->lookahead != '\n') {
                return false;
            }
            advance(lexer);
        } else {
            return true;
        }
    }
}

/// In a state shared by a function body and a braced initializer, a `}` closes an initializer
/// when the closing braces are followed by `;`, `,` or `)`.
static bool braces_close_initializer(TSLexer *lexer) {
    bool newline = false;
    while (lexer->lookahead == '}') {
        advance(lexer);
        if (!skip_trivia(lexer, &newline)) {
            return true;
        }
    }
    return lexer->lookahead == ';' || lexer->lookahead == ',' || lexer->lookahead == ')';
}

typedef enum { NOT_MACRO, MACRO_INVOCATION, MACRO_TYPE } MacroShape;

/// After `C(...) A` (consumed), with optional arguments of `A` next: true when a member
/// initializer list or `= default`/`= delete` follows, which ends a constructor declarator with a
/// suffix annotation `A` and never the declarator `A` of a function-like macro type `C(...)`.
static bool constructor_annotation_follows(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    if (lexer->lookahead == '(') {
        bool literal_argument = false;
        if (!skip_balanced_arguments(lexer, &literal_argument)) {
            return false;
        }
    }
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    int32_t c = lexer->lookahead;
    if (c != ':' && c != '=') {
        return false;
    }
    advance(lexer);
    if (c == ':') {
        return lexer->lookahead != ':';
    }
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    Word word;
    return read_word(lexer, &word) && (word_is(&word, "default") || word_is(&word, "delete"));
}

/// After callee `A` (consumed) and `(`: classify `A(...)` by what follows its balanced argument
/// list. It is a statement-like invocation when nothing can continue an expression or
/// declarator, and a type when a declarator follows on the same line.
static MacroShape classify_macro_call(TSLexer *lexer, bool initializer_possible, bool expression_possible) {
    bool literal_argument = false;
    if (!skip_balanced_arguments(lexer, &literal_argument)) {
        return NOT_MACRO;
    }
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return NOT_MACRO;
    }
    if (lexer->eof(lexer)) {
        return MACRO_INVOCATION;
    }
    int32_t c = lexer->lookahead;
    if (c == '}') {
        return !initializer_possible || !braces_close_initializer(lexer) ? MACRO_INVOCATION : NOT_MACRO;
    }
    // A parameter list never begins with a literal, so `A("x") {` and member-level `A(T, 0);`
    // cannot be constructor declarations.
    if (c == '{') {
        return literal_argument ? MACRO_INVOCATION : NOT_MACRO;
    }
    if (c == ';') {
        return literal_argument && !expression_possible ? MACRO_INVOCATION : NOT_MACRO;
    }
    Word word;
    if (!newline) {
        // `A(...) name` is never an expression; `A(...) *name` is one only where expressions may
        // start.
        if ((c == '*' || c == '&') && !expression_possible) {
            return MACRO_TYPE;
        }
        if (!read_word(lexer, &word) || word_in(&word, KEYWORDS) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
            return NOT_MACRO;
        }
        return constructor_annotation_follows(lexer) ? NOT_MACRO : MACRO_TYPE;
    }
    if (c == '#' || c == '~') {
        return MACRO_INVOCATION;
    }
    if (c == '[' || c == ':') {
        advance(lexer);
        return lexer->lookahead == c ? MACRO_INVOCATION : NOT_MACRO;
    }
    if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
        return NOT_MACRO;
    }
    if (word_in(&word, CONTINUATION_WORDS) || (!expression_possible && word_in(&word, DECLARATOR_SUFFIX_WORDS))) {
        return NOT_MACRO;
    }
    return MACRO_INVOCATION;
}

/// After a class key and callee `A` (consumed) at `(`: `A(...) Name` is an annotation call before
/// the class name, since an elaborated type's declarator is never followed by a name.
static bool class_annotation_follows(TSLexer *lexer) {
    bool literal_argument = false;
    bool newline = false;
    if (!skip_balanced_arguments(lexer, &literal_argument) || !skip_trivia(lexer, &newline)) {
        return false;
    }
    Word word;
    return read_word(lexer, &word) && !word_in(&word, KEYWORDS) && lexer->lookahead != '"' && lexer->lookahead != '\'';
}

/// From a class head's own `{` (not yet consumed): true once a `;` appears at the block's own
/// depth before any `,` at that depth, or before the block closes. Nested brackets, comments,
/// quoted and raw-string literals cannot unbalance or falsely trigger the scan. A braced
/// initializer's elements are always comma-separated expressions and never hold a bare top-level
/// `;`, so finding one proves this block is a member declaration list instead.
static bool class_body_members_follow(TSLexer *lexer) {
    unsigned depth = 0;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == '{' || c == '(' || c == '[') {
            depth++;
            advance(lexer);
        } else if (c == '}' || c == ')' || c == ']') {
            advance(lexer);
            if (--depth == 0) {
                return false;
            }
        } else if (depth == 1 && c == ';') {
            return true;
        } else if (depth == 1 && c == ',') {
            return false;
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            skip_comment_after_slash(lexer, &newline);
        } else if (c == '"' || c == '\'') {
            if (!skip_quoted(lexer)) {
                return false;
            }
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (lexer->lookahead == '"' && is_raw_prefix(&word) && !skip_raw_string(lexer)) {
                return false;
            }
        } else if (is_digit(c)) {
            while (is_identifier_part(lexer->lookahead) || lexer->lookahead == '.' || lexer->lookahead == '\'') {
                advance(lexer);
            }
        } else {
            advance(lexer);
        }
    }
    return false;
}

/// After a class key and a first bare identifier A (read; its end already marked): a second bare
/// identifier directly followed by `{` has no ordinary reading other than `A` annotating that
/// name, once the block it opens is certified a member declaration list. Stacked annotations and
/// a base-clause after the name fall back to the ordinary GLR reading.
static bool class_leading_annotation_follows(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    Word word;
    if (!is_identifier_start(lexer->lookahead) || !read_word(lexer, &word) || word_in(&word, KEYWORDS)) {
        return false;
    }
    skip_horizontal_space(lexer);
    return lexer->lookahead == '{' && class_body_members_follow(lexer);
}

/// Keywords that cannot follow an identifier in any declaration or expression, so a bare
/// identifier before them ends a statement-like macro invocation.
static const char *const STATEMENT_KEYWORDS[] = {
    "break", "case", "catch", "co_return", "co_yield", "continue", "default", "delete", "do", "else",
    "explicit", "for", "goto", "if", "namespace", "new", "private", "protected", "public", "return",
    "static_assert", "switch", "template", "using", "while", NULL,
};

/// After a word on its own line: true when a keyword type follows, possibly after specifiers.
/// `auto` counts only after a specifier, because `A auto` is a constrained placeholder.
/// Reports whether specifiers were consumed before the word left in `word`.
static bool keyword_declaration_follows(TSLexer *lexer, Word *word, bool *specified) {
    bool after_specifier = false;
    for (;;) {
        if (word_in(word, TYPE_KEYWORDS) || (after_specifier && is_placeholder_keyword(word))) {
            return true;
        }
        if (!word_in(word, SPECIFIER_KEYWORDS)) {
            *specified = after_specifier;
            return false;
        }
        after_specifier = true;
        while (is_space(lexer->lookahead)) {
            advance(lexer);
        }
        if (!read_word(lexer, word)) {
            return false;
        }
    }
}

/// From `{`, consume a balanced block, skipping literals and comments.
static bool skip_balanced_block(TSLexer *lexer) {
    unsigned depth = 0;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == '{') {
            depth++;
            advance(lexer);
        } else if (c == '}') {
            advance(lexer);
            if (--depth == 0) {
                return true;
            }
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            skip_comment_after_slash(lexer, &newline);
        } else if (c == '"' || c == '\'') {
            if (!skip_quoted(lexer)) {
                return false;
            }
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (lexer->lookahead == '"' && is_raw_prefix(&word) && !skip_raw_string(lexer)) {
                return false;
            }
        } else if (is_digit(c)) {
            while (is_identifier_part(lexer->lookahead) || lexer->lookahead == '.' || lexer->lookahead == '\'') {
                advance(lexer);
            }
        } else {
            advance(lexer);
        }
    }
    return false;
}

/// From `<`, consume balanced template arguments on the way to a declarator.
static bool skip_template_arguments(TSLexer *lexer) {
    unsigned depth = 0;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        int32_t c = lexer->lookahead;
        if (lexer->eof(lexer) || c == ';' || c == '{' || c == '}') {
            return false;
        }
        advance(lexer);
        if (c == '<') {
            depth++;
        } else if (c == '>' && --depth == 0) {
            return true;
        } else if (c == '"' || c == '\'') {
            return false;
        }
    }
    return false;
}

/// Calling conventions that may stand between a declared type and its declarator.
static const char *const CALLING_CONVENTIONS[] = {
    "__cdecl", "__clrcall", "__fastcall", "__stdcall", "__thiscall", "__vectorcall", NULL,
};

/// After a type name's first word (consumed): consume its `::` qualifiers and template arguments,
/// then certify that a declarator follows: cv-qualifiers, calling conventions, `*` and `&`, then
/// a name. A `(` right after the type is left unclaimed, since `A ns::f(` and `A f<T>(` are
/// ordinary declarators of type `A`. Only a qualified or templated type, which a declarator
/// cannot continue, may put its declarator on a later line.
static bool type_declarator_follows(TSLexer *lexer, bool qualified) {
    Word word;
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '<') {
            if (!skip_template_arguments(lexer)) {
                return false;
            }
            qualified = true;
        } else if (lexer->lookahead == ':') {
            advance(lexer);
            if (lexer->lookahead != ':') {
                return false;
            }
            advance(lexer);
            skip_horizontal_space(lexer);
            if (!read_word(lexer, &word) || (word_in(&word, KEYWORDS) && !word_in(&word, TYPE_KEYWORDS))) {
                return false;
            }
            qualified = true;
        } else {
            break;
        }
    }
    for (;;) {
        bool newline = false;
        if (!skip_trivia(lexer, &newline) || (newline && !qualified)) {
            return false;
        }
        if (lexer->lookahead == '*' || lexer->lookahead == '&') {
            advance(lexer);
            continue;
        }
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (word_is(&word, "const") || word_is(&word, "volatile") || word_in(&word, CALLING_CONVENTIONS)) {
            continue;
        }
        return !word_in(&word, KEYWORDS) || word_is(&word, "operator");
    }
}

/// After `A` (already consumed): certify `A [B ...] [specifier keywords]` followed on one physical
/// line by a keyword type, by a qualified or templated type and its declarator, or, after a
/// specifier keyword, by any type name and its declarator. `A B` alone stays a type and its
/// declarator, so a plain type name directly after annotations is never claimed.
static bool leading_annotation_follows(TSLexer *lexer) {
    if (lexer->lookahead == ':' || lexer->lookahead == '<') {
        // `A::` and `A<` begin a qualified or templated name.
        return false;
    }
    Word word;
    bool after_specifier = false;
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':') {
            return type_declarator_follows(lexer, true);
        }
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (word_in(&word, TYPE_KEYWORDS) || (after_specifier && is_placeholder_keyword(&word))) {
            return true;
        }
        if (word_in(&word, SPECIFIER_KEYWORDS)) {
            after_specifier = true;
            continue;
        }
        if (word_in(&word, KEYWORDS)) {
            return false;
        }
        if (after_specifier) {
            return type_declarator_follows(lexer, false);
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':' || lexer->lookahead == '<') {
            return type_declarator_follows(lexer, true);
        }
    }
}

/// True when the next token, at the start of a later line, begins a new statement or
/// declaration rather than continuing the previous one.
static bool new_item_starts(TSLexer *lexer) {
    if (lexer->eof(lexer) || lexer->lookahead == '}' || lexer->lookahead == '{' || lexer->lookahead == '~') {
        return true;
    }
    Word word;
    if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
        return false;
    }
    return !word_in(&word, CONTINUATION_WORDS);
}

/// Consume the rest of a directive line, including continuations and comments spanning lines.
static void skip_directive_line(TSLexer *lexer) {
    while (!lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        if (c == '\n') {
            advance(lexer);
            return;
        }
        if (c == '\\') {
            advance(lexer);
            if (lexer->lookahead == '\r') {
                advance(lexer);
            }
            if (lexer->lookahead == '\n') {
                advance(lexer);
            }
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            skip_comment_after_slash(lexer, &newline);
        } else if (c == '"' || c == '\'') {
            skip_quoted(lexer);
        } else {
            advance(lexer);
        }
    }
}

/// From `#`, consume a directive after which the same preprocessing path continues; false at a
/// directive that switches to an alternative branch.
static bool skip_path_directive(TSLexer *lexer) {
    advance(lexer);
    skip_horizontal_space(lexer);
    Word word;
    if (read_word(lexer, &word) && (word_is(&word, "else") || word_is(&word, "elif") ||
                                    word_is(&word, "elifdef") || word_is(&word, "elifndef"))) {
        return false;
    }
    skip_directive_line(lexer);
    return true;
}

/// After bare `A` (consumed) followed by a line break or `}`: certify a statement-like macro
/// only when `A` cannot begin or end any ordinary expression or declaration.
static bool bare_macro_follows(TSLexer *lexer, bool initializer_possible) {
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    while (newline && lexer->lookahead == '#') {
        if (!skip_path_directive(lexer) || !skip_trivia(lexer, &newline)) {
            return false;
        }
    }
    if (lexer->eof(lexer)) {
        return newline;
    }
    int32_t c = lexer->lookahead;
    if (c == '}') {
        return !initializer_possible || !braces_close_initializer(lexer);
    }
    if (c == '{') {
        // `A {...}` followed by `;` or an operator is a compound literal expression; inside a
        // possible braced initializer it may be an element.
        bool after_block = false;
        if (initializer_possible || !skip_balanced_block(lexer) || !skip_trivia(lexer, &after_block)) {
            return false;
        }
        return after_block && (lexer->lookahead == '#' || new_item_starts(lexer));
    }
    if (!newline) {
        return false;
    }
    if (c == '~') {
        return true;
    }
    if (c == '[') {
        advance(lexer);
        return lexer->lookahead == '[';
    }
    Word word;
    if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
        return false;
    }
    bool specified = false;
    if (word_in(&word, STATEMENT_KEYWORDS) || keyword_declaration_follows(lexer, &word, &specified)) {
        return true;
    }
    if (word_in(&word, KEYWORDS)) {
        return false;
    }
    // `A` then `B` on a later line: `A B` would already be a complete declaration, so `A` is a
    // macro when `B` (possibly qualified or templated) is followed by a declarator token, or
    // by a later line that cannot continue `A B`. `A B(...)` without a terminator is likewise
    // no declaration when `B(...)` is itself a statement-like invocation. After specifiers,
    // `A static B x` declares with its own type `B`, so `A` cannot be a type either.
    skip_horizontal_space(lexer);
    if (lexer->lookahead == '(') {
        return !specified && classify_macro_call(lexer, false, true) == MACRO_INVOCATION;
    }
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':') {
            advance(lexer);
            if (lexer->lookahead != ':') {
                return false;
            }
            advance(lexer);
            skip_horizontal_space(lexer);
            if (!read_word(lexer, &word)) {
                return false;
            }
        } else if (lexer->lookahead == '<') {
            if (!skip_template_arguments(lexer)) {
                return false;
            }
        } else {
            break;
        }
    }
    if (lexer->lookahead == '*' || lexer->lookahead == '&') {
        return true;
    }
    if (is_identifier_start(lexer->lookahead)) {
        return read_word(lexer, &word) && !word_in(&word, CONTINUATION_WORDS);
    }
    bool after_second = false;
    if (!skip_trivia(lexer, &after_second) || !after_second) {
        return false;
    }
    return new_item_starts(lexer);
}

/// Conditional groups larger than this are left to the structured rules when those rules apply.
#define MAX_STRUCTURED_CONDITIONAL_SCAN 8192

typedef struct {
    bool complete;
    /// Some branch is unbalanced or starts by continuing an earlier construct.
    bool irregular;
    /// An `#else`/`#elif` exists; the token then extends through the last such line.
    bool alternative;
} ConditionalGroup;

/// From the line after an opening `#if`, scan its group to the matching `#endif`. Nested groups
/// count only their first branch toward bracket balance, as a single preprocessing pass would.
static void scan_conditional_group(TSLexer *lexer, unsigned budget, ConditionalGroup *group) {
    unsigned nesting = 0;
    unsigned skipping = 0;
    int depth = 0;
    bool line_start = true;
    bool branch_start = true;
    for (; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return;
        }
        int32_t c = lexer->lookahead;
        if (c == '\n') {
            line_start = true;
            advance(lexer);
            continue;
        }
        if (is_space(c) || c == '\\') {
            advance(lexer);
            continue;
        }
        if (c == '#' && line_start) {
            advance(lexer);
            skip_horizontal_space(lexer);
            Word word;
            read_word(lexer, &word);
            if (word_is(&word, "if") || word_is(&word, "ifdef") || word_is(&word, "ifndef")) {
                nesting++;
            } else if (word_is(&word, "endif")) {
                if (nesting == 0) {
                    group->irregular = group->irregular || depth != 0;
                    group->complete = true;
                    return;
                }
                if (skipping == nesting) {
                    skipping = 0;
                }
                nesting--;
            } else if (word_is(&word, "else") || word_is(&word, "elif") || word_is(&word, "elifdef") ||
                       word_is(&word, "elifndef")) {
                if (nesting == 0) {
                    group->irregular = group->irregular || depth != 0;
                    group->alternative = true;
                    depth = 0;
                    branch_start = true;
                    skip_directive_line(lexer);
                    lexer->mark_end(lexer);
                    line_start = true;
                    continue;
                }
                if (skipping == 0) {
                    skipping = nesting;
                }
            }
            skip_directive_line(lexer);
            line_start = true;
            continue;
        }
        line_start = false;
        bool first = branch_start && nesting == 0;
        if (c == '/') {
            bool newline = false;
            advance(lexer);
            if (skip_comment_after_slash(lexer, &newline)) {
                continue;
            }
            group->irregular = group->irregular || first;
            branch_start = false;
            continue;
        }
        branch_start = false;
        bool counted = skipping == 0;
        if (c == '(' || c == '[' || c == '{') {
            depth += counted;
            advance(lexer);
        } else if (c == ')' || c == ']' || c == '}') {
            depth -= counted;
            if (depth < 0 && nesting == 0) {
                group->irregular = true;
            }
            advance(lexer);
        } else if (c == '"' || c == '\'') {
            skip_quoted(lexer);
        } else if (is_digit(c)) {
            int32_t previous = 0;
            while (is_identifier_part(lexer->lookahead) || lexer->lookahead == '.' || lexer->lookahead == '\'' ||
                   ((lexer->lookahead == '+' || lexer->lookahead == '-') &&
                    (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P'))) {
                previous = lexer->lookahead;
                advance(lexer);
            }
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (first && (word_is(&word, "else") || word_is(&word, "catch"))) {
                group->irregular = true;
            }
            if (lexer->lookahead == '"' && is_raw_prefix(&word)) {
                skip_raw_string(lexer);
            } else if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                skip_quoted(lexer);
            }
        } else {
            advance(lexer);
            if (first) {
                // Binary, member, separator and conditional operators only continue a construct.
                int32_t next = lexer->lookahead;
                group->irregular = group->irregular || c == ',' || c == '|' || c == '?' || c == '.' || c == '<' ||
                                   c == '>' || c == '=' || c == '%' || c == '^' || (c == '&' && next == '&') ||
                                   (c == '-' && next == '>') || (c == ':' && next != ':');
            }
        }
    }
}

/// From `#`, take a conditional in place when its structured rule cannot parse it here: the
/// directive occurs mid-construct, or a branch is unbalanced or continues an earlier construct.
/// The last branch is parsed; the directive lines and earlier branches form one opaque token.
/// Every conditional nested in a branch taken in place is also taken in place, so each `#endif`
/// closes the most recent such conditional exactly.
static bool scan_preproc_conditional(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    advance(lexer);
    skip_horizontal_space(lexer);
    Word word;
    if (!read_word(lexer, &word)) {
        return false;
    }
    if (word_is(&word, "endif")) {
        if (scanner->inline_conditionals == 0) {
            return false;
        }
        skip_directive_line(lexer);
        lexer->mark_end(lexer);
        scanner->inline_conditionals--;
        lexer->result_symbol = PREPROC_INLINE_CONDITIONAL;
        return true;
    }
    if (!word_is(&word, "if") && !word_is(&word, "ifdef") && !word_is(&word, "ifndef")) {
        return false;
    }
    if (scanner->inline_conditionals == UINT8_MAX) {
        return false;
    }
    bool structured = valid_symbols[PREPROC_IF_PROBE] && scanner->inline_conditionals == 0;
    skip_directive_line(lexer);
    lexer->mark_end(lexer);
    ConditionalGroup group = {false, false, false};
    scan_conditional_group(lexer, structured ? MAX_STRUCTURED_CONDITIONAL_SCAN : MAX_INVOCATION_SCAN, &group);
    if (!group.complete || (structured && !group.irregular)) {
        return false;
    }
    scanner->inline_conditionals++;
    lexer->result_symbol = group.alternative ? PREPROC_INACTIVE_BRANCH : PREPROC_INLINE_CONDITIONAL;
    return true;
}

/// Upper bound on annotations stacked after one function declarator's parameters.
#define MAX_FUNCTION_SUFFIX_ANNOTATIONS 8

/// After a function declarator's parameters and annotation `A` (consumed): certify `A` when it,
/// with optional balanced arguments and further such annotations, is followed by a token that ends
/// or continues a function declarator: a body or initializer list, `;`, `=`, `->`, `[[` or a
/// declarator-suffix keyword. An identifier can never follow the parameters in ordinary syntax.
/// Two adjacent bare words stay unclaimed: like `T name(...)` after a misread leading annotation
/// call, or a K&R parameter declaration `T name;`, they may be a type and its declarator.
static bool function_suffix_follows(TSLexer *lexer) {
    unsigned bare_words = 1;
    for (unsigned count = 0; count < MAX_FUNCTION_SUFFIX_ANNOTATIONS; count++) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '(') {
            bool literal_argument = false;
            if (!skip_balanced_arguments(lexer, &literal_argument)) {
                return false;
            }
            bare_words = 0;
        }
        bool newline = false;
        if (!skip_trivia(lexer, &newline) || lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == '{' || c == ';' || c == ',') {
            return true;
        }
        if (c == ':' || c == '=' || c == '-' || c == '[') {
            advance(lexer);
            int32_t next = lexer->lookahead;
            return c == '-' ? next == '>' : c == '[' ? next == '[' : next != c;
        }
        Word word;
        if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
            return false;
        }
        if (word_in(&word, KEYWORDS)) {
            return word_in(&word, FUNCTION_SUFFIX_KEYWORDS);
        }
        if (++bare_words > 1) {
            return false;
        }
    }
    return false;
}

/// At the start of a conditional directive's macro argument: consume the argument through its
/// end, a `,` or `)` at its own depth on the logical line, and certify it only when it holds a
/// character that no preprocessor expression can, such as the `?` of `F(?)` or the `::` of
/// `__has_cpp_attribute(gnu::cold)`. Other arguments stay ordinary expressions.
static bool scan_preproc_raw_argument(TSLexer *lexer) {
    while (is_horizontal_space(lexer->lookahead)) {
        lexer->advance(lexer, true);
    }
    // `__has_include(<header>)` keeps its header name.
    if (lexer->lookahead == '<') {
        return false;
    }
    unsigned depth = 0;
    bool opaque = false;
    bool empty = true;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        int32_t c = lexer->lookahead;
        if (lexer->eof(lexer) || c == '\n') {
            return false;
        }
        if (depth == 0 && (c == ',' || c == ')')) {
            return opaque && !empty;
        }
        if (c == '\\') {
            advance(lexer);
            if (lexer->lookahead == '\r') {
                advance(lexer);
            }
            if (lexer->lookahead != '\n') {
                return false;
            }
            advance(lexer);
            continue;
        }
        if (c == '"' || c == '\'') {
            if (!skip_quoted(lexer)) {
                return false;
            }
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            if (skip_comment_after_slash(lexer, &newline)) {
                if (newline) {
                    return false;
                }
                continue;
            }
            empty = false;
            lexer->mark_end(lexer);
            continue;
        } else {
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
            } else if (c == '=') {
                advance(lexer);
                opaque = opaque || lexer->lookahead != '=';
                empty = false;
                lexer->mark_end(lexer);
                continue;
            } else if (c == '?' || c == ':' || c == '.' || c == '[' || c == ']' || c == '{' || c == '}' || c == ';' ||
                       c == '@' || c == '$' || c == '#' || c == '`') {
                opaque = true;
            }
            advance(lexer);
        }
        if (!is_space(c)) {
            empty = false;
            lexer->mark_end(lexer);
        }
    }
    return false;
}

/// Certify the identifier just read, whose end is marked, as an annotation or macro token.
/// At a class member's start, after a word and before its `:`: certify the word as a section
/// label, a macro standing for an access specifier as in Qt's `signals:`, when a single `:` ends
/// the line. An unnamed bit-field `T : 3;` has its width on the same line, and no other member
/// starts with a word and a single `:`.
static bool section_label_follows(TSLexer *lexer) {
    advance(lexer);
    if (lexer->lookahead == ':') {
        return false;
    }
    bool newline = false;
    return skip_trivia(lexer, &newline) && (newline || lexer->eof(lexer));
}

static bool scan_certified_identifier(TSLexer *lexer, const bool *valid_symbols, bool newline_before) {
    if (valid_symbols[FUNCTION_SUFFIX_ANNOTATION]) {
        lexer->result_symbol = FUNCTION_SUFFIX_ANNOTATION;
        return function_suffix_follows(lexer);
    }
    skip_horizontal_space(lexer);
    if (lexer->lookahead == ':' && valid_symbols[SECTION_LABEL]) {
        lexer->result_symbol = SECTION_LABEL;
        return section_label_follows(lexer);
    }
    if (lexer->lookahead == '(') {
        if (valid_symbols[JUXTAPOSED_CALLEE]) {
            // After a call expression nothing ordinary can begin with a word, so `A(x) B(y)` juxtaposes
            // `B(y)` once its arguments are balanced.
            bool literal_argument = false;
            lexer->result_symbol = JUXTAPOSED_CALLEE;
            return skip_balanced_arguments(lexer, &literal_argument);
        }
        if (valid_symbols[CLASS_ANNOTATION_CALLEE]) {
            lexer->result_symbol = CLASS_ANNOTATION_CALLEE;
            return class_annotation_follows(lexer);
        }
        if (!valid_symbols[MACRO_INVOCATION_CALLEE] && !valid_symbols[MACRO_TYPE_CALLEE]) {
            return false;
        }
        switch (classify_macro_call(lexer, valid_symbols[INITIALIZER_START_PROBE],
                                    valid_symbols[EXPRESSION_START_PROBE])) {
            case MACRO_INVOCATION:
                lexer->result_symbol = MACRO_INVOCATION_CALLEE;
                return valid_symbols[MACRO_INVOCATION_CALLEE];
            case MACRO_TYPE:
                lexer->result_symbol = MACRO_TYPE_CALLEE;
                return valid_symbols[MACRO_TYPE_CALLEE];
            default:
                return false;
        }
    }
    if (valid_symbols[MACRO_INVOCATION_CALLEE] &&
        (lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->lookahead == '/' || lexer->lookahead == '}' ||
         lexer->lookahead == '{')) {
        lexer->result_symbol = MACRO_INVOCATION_CALLEE;
        return bare_macro_follows(lexer, valid_symbols[INITIALIZER_START_PROBE]);
    }
    if (valid_symbols[CLASS_LEADING_ANNOTATION]) {
        lexer->result_symbol = CLASS_LEADING_ANNOTATION;
        return class_leading_annotation_follows(lexer);
    }
    if (valid_symbols[LEADING_ANNOTATION]) {
        // A declaration may start here, so `A B` is an ordinary type and declarator.
        lexer->result_symbol = LEADING_ANNOTATION;
        return leading_annotation_follows(lexer);
    }
    // A declarator annotation shares a line with the preceding token, so recovery after a
    // skipped `}` cannot turn the next declaration's type into an annotation.
    if (valid_symbols[DECLARATOR_ANNOTATION] && !valid_symbols[EXPRESSION_START_PROBE] && !newline_before) {
        lexer->result_symbol = DECLARATOR_ANNOTATION;
        return declarator_annotation_follows(lexer);
    }
    return false;
}

/// One identifier read serves every identifier-based token, because a scan cannot rewind. Where an
/// identifier is valid, a word that is no certified token is returned as that identifier rather
/// than read again by the lexer. Only words an ordinary identifier would end at qualify: ASCII,
/// not a keyword, and not the prefix of a literal or of a universal character name.
static bool scan_identifier_token(TSLexer *lexer, const bool *valid_symbols, bool newline_before, bool certify,
                                  bool identifier) {
    Word word;
    if (!read_word(lexer, &word) || word_in(&word, KEYWORDS)) {
        return false;
    }
    int32_t next = lexer->lookahead;
    bool plain = identifier && !word.non_ascii && next != '"' && next != '\'' && next != '\\';
    lexer->mark_end(lexer);
    if (certify && scan_certified_identifier(lexer, valid_symbols, newline_before)) {
        return true;
    }
    if (plain) {
        lexer->result_symbol = IDENTIFIER;
        return true;
    }
    return false;
}

/// Scan the raw string delimiter in R"delimiter(content)delimiter"
static bool scan_raw_string_delimiter(Scanner *scanner, TSLexer *lexer) {
    if (scanner->delimiter_length > 0) {
        // Closing delimiter: must exactly match the opening delimiter.
        // We already checked this when scanning content, but this is how we
        // know when to stop. We can't stop at ", because R"""hello""" is valid.
        for (int i = 0; i < scanner->delimiter_length; ++i) {
            if (lexer->lookahead != scanner->delimiter[i]) {
                return false;
            }
            advance(lexer);
        }
        reset(scanner);
        return true;
    }

    // Opening delimiter: record the d-char-sequence up to (.
    // d-char is any basic character except parens, backslashes, and spaces.
    for (;;) {
        if (scanner->delimiter_length >= MAX_DELIMITER_LENGTH || lexer->eof(lexer) || lexer->lookahead == '\\' ||
            is_space(lexer->lookahead)) {
            return false;
        }
        if (lexer->lookahead == '(') {
            // Rather than create a token for an empty delimiter, we fail and
            // let the grammar fall back to a delimiter-less rule.
            return scanner->delimiter_length > 0;
        }
        scanner->delimiter[scanner->delimiter_length++] = lexer->lookahead;
        advance(lexer);
    }
}

/// Scan the raw string content in R"delimiter(content)delimiter"
static bool scan_raw_string_content(Scanner *scanner, TSLexer *lexer) {
    // The progress made through the delimiter since the last ')'.
    // The delimiter may not contain ')' so a single counter suffices.
    for (int delimiter_index = -1;;) {
        // If we hit EOF, consider the content to terminate there.
        // This forms an incomplete raw_string_literal, and models the code
        // well.
        if (lexer->eof(lexer)) {
            lexer->mark_end(lexer);
            return true;
        }

        if (delimiter_index >= 0) {
            if (delimiter_index == scanner->delimiter_length) {
                if (lexer->lookahead == '"') {
                    return true;
                }
                delimiter_index = -1;
            } else {
                if (lexer->lookahead == scanner->delimiter[delimiter_index]) {
                    delimiter_index += 1;
                } else {
                    delimiter_index = -1;
                }
            }
        }

        if (delimiter_index == -1 && lexer->lookahead == ')') {
            // The content doesn't include the )delimiter" part.
            // We must still scan through it, but exclude it from the token.
            lexer->mark_end(lexer);
            delimiter_index = 0;
        }

        advance(lexer);
    }
}

void *tree_sitter_cpp_external_scanner_create() {
    Scanner *scanner = (Scanner *)ts_calloc(1, sizeof(Scanner));
    memset(scanner, 0, sizeof(Scanner));
    return scanner;
}

bool tree_sitter_cpp_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    if (valid_symbols[RAW_STRING_DELIMITER] && valid_symbols[RAW_STRING_CONTENT]) {
        // we're in error recovery
        return false;
    }

    // No skipping leading whitespace: raw-string grammar is space-sensitive.
    if (valid_symbols[RAW_STRING_DELIMITER]) {
        lexer->result_symbol = RAW_STRING_DELIMITER;
        return scan_raw_string_delimiter(scanner, lexer);
    }

    if (valid_symbols[RAW_STRING_CONTENT]) {
        lexer->result_symbol = RAW_STRING_CONTENT;
        return scan_raw_string_content(scanner, lexer);
    }

    if (valid_symbols[PREPROC_RAW_ARGUMENT]) {
        lexer->result_symbol = PREPROC_RAW_ARGUMENT;
        return scan_preproc_raw_argument(lexer);
    }

    bool identifier_tokens = valid_symbols[LEADING_ANNOTATION] || valid_symbols[DECLARATOR_ANNOTATION] ||
                             valid_symbols[MACRO_INVOCATION_CALLEE] || valid_symbols[MACRO_TYPE_CALLEE] ||
                             valid_symbols[CLASS_ANNOTATION_CALLEE] || valid_symbols[CLASS_LEADING_ANNOTATION] ||
                             valid_symbols[FUNCTION_SUFFIX_ANNOTATION] || valid_symbols[JUXTAPOSED_CALLEE] ||
                             valid_symbols[SECTION_LABEL];
    if (valid_symbols[PREPROC_END_OF_INPUT]) {
        // A directive line still needs its end: a newline, or end of input on the last line.
        while (is_horizontal_space(lexer->lookahead) || lexer->lookahead == '\r') {
            lexer->advance(lexer, true);
        }
        if (!lexer->eof(lexer)) {
            return false;
        }
        lexer->result_symbol = PREPROC_END_OF_INPUT;
        return true;
    }
    bool conditional_tokens = valid_symbols[PREPROC_INLINE_CONDITIONAL];
    if (!identifier_tokens && !conditional_tokens && !valid_symbols[IDENTIFIER]) {
        return false;
    }
    bool newline_before = false;
    bool space_before = false;
    for (;;) {
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            newline_before = true;
        } else if (lexer->lookahead == '\\') {
            // A line continuation is whitespace that keeps the logical line.
            lexer->advance(lexer, true);
            if (lexer->lookahead == '\r') {
                lexer->advance(lexer, true);
            }
            if (lexer->lookahead != '\n') {
                return false;
            }
        } else if (!is_space(lexer->lookahead)) {
            break;
        }
        space_before = true;
        lexer->advance(lexer, true);
    }
    if (lexer->lookahead == '#') {
        return conditional_tokens && scan_preproc_conditional(scanner, lexer, valid_symbols);
    }
    // A literal suffix is immediate, so after a space only an identifier can match the word.
    bool identifier = valid_symbols[IDENTIFIER] && (space_before || !valid_symbols[LITERAL_SUFFIX]);
    if (!identifier_tokens && !identifier) {
        return false;
    }
    return scan_identifier_token(lexer, valid_symbols, newline_before, identifier_tokens, identifier);
}

unsigned tree_sitter_cpp_external_scanner_serialize(void *payload, char *buffer) {
    static_assert(1 + MAX_DELIMITER_LENGTH * sizeof(wchar_t) < TREE_SITTER_SERIALIZATION_BUFFER_SIZE,
                  "Serialized delimiter is too long!");

    Scanner *scanner = (Scanner *)payload;
    // The default state must serialize empty: tree-sitter merges GLR versions and reuses
    // incremental nodes only when external states are byte-equal, and "no external token"
    // is the empty state.
    if (scanner->inline_conditionals == 0 && scanner->delimiter_length == 0) {
        return 0;
    }
    buffer[0] = (char)scanner->inline_conditionals;
    size_t size = scanner->delimiter_length * sizeof(wchar_t);
    memcpy(buffer + 1, scanner->delimiter, size);
    return (unsigned)(1 + size);
}

void tree_sitter_cpp_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    if (length == 0) {
        scanner->inline_conditionals = 0;
        scanner->delimiter_length = 0;
        return;
    }
    assert((length - 1) % sizeof(wchar_t) == 0 && "Can't decode serialized delimiter!");
    scanner->inline_conditionals = (uint8_t)buffer[0];
    scanner->delimiter_length = (length - 1) / sizeof(wchar_t);
    if (length > 1) {
        memcpy(&scanner->delimiter[0], buffer + 1, length - 1);
    }
}

void tree_sitter_cpp_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    ts_free(scanner);
}
