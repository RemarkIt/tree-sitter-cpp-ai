// Word lists. Each must stay strictly ASCII-sorted; lab/tests/test_scanner_words.py checks them.
// Part of scanner.c, which includes this file once after its token and state definitions.

#ifndef TREE_SITTER_CPP_SCANNER_WORDS_H
#define TREE_SITTER_CPP_SCANNER_WORDS_H

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

/// Keywords that cannot follow an identifier in any declaration or expression, so a bare
/// identifier before them ends a statement-like macro invocation.
static const char *const STATEMENT_KEYWORDS[] = {
    "break", "case", "catch", "co_return", "co_yield", "continue", "default", "delete", "do", "else",
    "explicit", "for", "goto", "if", "namespace", "new", "private", "protected", "public", "return",
    "static_assert", "switch", "template", "typedef", "using", "while", NULL,
};

/// Calling conventions that may stand between a declared type and its declarator.
static const char *const CALLING_CONVENTIONS[] = {
    "__cdecl", "__clrcall", "__fastcall", "__stdcall", "__thiscall", "__vectorcall", NULL,
};

/// Statement keywords whose header `(...)` must be followed by a body.
static const char *const HEADER_KEYWORDS[] = {"for", "if", "switch", "while", NULL};

/// `auto`/`decltype` directly after an identifier form a constrained placeholder, but after a
/// specifier keyword they can only begin the declared type.
static inline bool is_placeholder_keyword(const Word *word) {
    return !word->truncated && (strcmp(word->text, "auto") == 0 || strcmp(word->text, "decltype") == 0);
}

/// `module` and `import` are keywords only at the start of a module or import declaration, so
/// elsewhere they name things, as in `_In_ HMODULE module`.
static inline bool is_contextual_name(const Word *word) {
    return word_is(word, "module") || word_is(word, "import");
}

/// Keywords naming a fundamental type, which no expression operand can be followed by a name.
static inline bool is_fundamental_type_word(const Word *word) {
    return word_in(word, TYPE_KEYWORDS) && !word_is(word, "class") && !word_is(word, "enum") &&
           !word_is(word, "struct") && !word_is(word, "typename") && !word_is(word, "union");
}

/// Directives opening a conditional group.
static const char *const CONDITIONAL_OPENINGS[] = {"if", "ifdef", "ifndef", NULL};

/// Directives switching a conditional group to its next branch.
static const char *const CONDITIONAL_ALTERNATIVES[] = {"elif", "elifdef", "elifndef", "else", NULL};

/// Directives that never begin a declaration's continuation on the line after a bare macro.
static const char *const LINE_ENDING_DIRECTIVES[] = {"define", "endif", "include", "pragma", "undef", NULL};

/// `const` or `volatile`.
static inline bool is_cv_qualifier(const Word *word) {
    return word_is(word, "const") || word_is(word, "volatile");
}

/// A word that may name a type or a scope: any non-keyword, or a keyword type.
static inline bool can_name_type(const Word *word) {
    return !word_in(word, KEYWORDS) || word_in(word, TYPE_KEYWORDS);
}

#endif // TREE_SITTER_CPP_SCANNER_WORDS_H
