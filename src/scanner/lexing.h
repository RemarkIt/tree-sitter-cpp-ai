// Characters, words, trivia and literals: the primitives every look-ahead scan is built from.
// Part of scanner.c, which includes this file once after its token and state definitions.

#ifndef TREE_SITTER_CPP_SCANNER_LEXING_H
#define TREE_SITTER_CPP_SCANNER_LEXING_H

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

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

static inline bool is_horizontal_space(int32_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

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

/// Upper bound on bytes examined after a candidate callee, keeping scans linear in practice.
#define MAX_INVOCATION_SCAN 65536

static inline void skip_horizontal_space(TSLexer *lexer) {
    while (is_horizontal_space(lexer->lookahead)) {
        advance(lexer);
    }
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

/// From `\`, consume a line continuation; false if the backslash does not end its line.
static bool skip_line_continuation(TSLexer *lexer) {
    advance(lexer);
    if (lexer->lookahead == '\r') {
        advance(lexer);
    }
    if (lexer->lookahead != '\n') {
        return false;
    }
    advance(lexer);
    return true;
}

/// From `:`, consume a scope operator `::`; false after a lone `:`.
static bool skip_scope_operator(TSLexer *lexer) {
    advance(lexer);
    if (lexer->lookahead != ':') {
        return false;
    }
    advance(lexer);
    return true;
}

/// From a digit, consume a number literal, including digit separators and signed exponents.
static void skip_number(TSLexer *lexer) {
    int32_t previous = 0;
    while (is_identifier_part(lexer->lookahead) || lexer->lookahead == '.' || lexer->lookahead == '\'' ||
           ((lexer->lookahead == '+' || lexer->lookahead == '-') &&
            (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P'))) {
        previous = lexer->lookahead;
        advance(lexer);
    }
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
            if (!skip_line_continuation(lexer)) {
                return false;
            }
        } else {
            return true;
        }
    }
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

/// After a word (read into `word`): consume the string or character literal it prefixes, as in
/// `u8"x"`, `L'x'` or `R"(x)"`; false if that literal is unterminated.
static bool skip_prefixed_literal(TSLexer *lexer, const Word *word) {
    if (lexer->lookahead == '"' && is_raw_prefix(word)) {
        return skip_raw_string(lexer);
    }
    if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
        return skip_quoted(lexer);
    }
    return true;
}

/// Consume one element that opens or closes no bracket: a comment or lone `/`, a literal, a word
/// with any literal it prefixes, a number, or any other single character. False at an
/// unterminated literal.
static bool skip_opaque_element(TSLexer *lexer) {
    int32_t c = lexer->lookahead;
    if (c == '/') {
        bool newline = false;
        advance(lexer);
        skip_comment_after_slash(lexer, &newline);
    } else if (c == '"' || c == '\'') {
        return skip_quoted(lexer);
    } else if (is_identifier_start(c)) {
        Word word;
        read_word(lexer, &word);
        return skip_prefixed_literal(lexer, &word);
    } else if (is_digit(c)) {
        skip_number(lexer);
    } else {
        advance(lexer);
    }
    return true;
}

#endif // TREE_SITTER_CPP_SCANNER_LEXING_H
