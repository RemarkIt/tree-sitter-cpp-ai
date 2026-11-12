#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"
#include <assert.h>
#include <string.h>
#include <wctype.h>

// The external scanner certifies macro-shaped syntax from the source bytes alone. Each token's
// grammar contract is documented at its declaration in grammar.js `externals`; the comments here
// name the function that decides it. Probes are never produced: their validity tells the scanner
// which readings the parser could take at this position.
//
// Modules, included below in dependency order:
//   scanner/lexing.h        characters, words, trivia and literals
//   scanner/words.h         keyword and directive word lists
//   scanner/balanced.h      balanced brackets, blocks, template arguments and argument lists
//   scanner/conditionals.h  preprocessor conditionals taken in place, raw directive arguments
//   scanner/declarations.h  annotation and macro certification in declarations
//   scanner/raw_strings.h   raw string literals

enum TokenType {
    RAW_STRING_DELIMITER,         // scan_raw_string_delimiter
    RAW_STRING_CONTENT,           // scan_raw_string_content
    LEADING_ANNOTATION,           // leading_annotation_follows
    DECLARATOR_ANNOTATION,        // declarator_annotation_follows
    EXPRESSION_START_PROBE,       // probe
    MACRO_INVOCATION_CALLEE,      // classify_macro_call, or bare_macro_follows without arguments
    INITIALIZER_START_PROBE,      // probe
    MACRO_TYPE_CALLEE,            // classify_macro_call
    PREPROC_INLINE_CONDITIONAL,   // scan_preproc_conditional
    PREPROC_INACTIVE_BRANCH,      // scan_preproc_conditional
    PREPROC_IF_PROBE,             // probe
    PREPROC_END_OF_INPUT,         // scan_preproc_end_of_input
    CLASS_ANNOTATION_CALLEE,      // class_annotation_follows
    CLASS_LEADING_ANNOTATION,     // class_leading_annotation_follows
    FUNCTION_SUFFIX_ANNOTATION,   // function_suffix_follows
    JUXTAPOSED_CALLEE,            // scan_certified_identifier
    PREPROC_RAW_ARGUMENT,         // scan_preproc_raw_argument
    SECTION_LABEL,                // section_label_follows
    LEADING_ANNOTATION_CALLEE,    // classify_macro_call
    ANNOTATION_ARGUMENTS,         // scan_annotation_arguments
    TYPENAME_CAST,                // dependent_cast_follows
    TEMPLATE_ARGUMENT_PROBE,      // probe
    IDENTIFIER,                   // scan_identifier_token
    LITERAL_SUFFIX,               // probe
    MEMBER_POINTER_SCOPE,         // member_pointer_follows
    MACRO_HEADER_CALLEE,          // classify_macro_call
    UNCERTAIN_ARGUMENT_LIST,      // scan_uncertain_argument_list
    PARAMETER_LIST_PROBE,         // probe
    CLASS_KEY_MACRO,              // annotation_tail_follows
    REF_NEW,                      // scan_identifier_token
    ENUMERATOR_ANNOTATION_CALLEE, // enumerator_annotation_tail_follows
};

/// Tokens certified from an identifier the scanner reads, by what follows it.
static const enum TokenType CERTIFIED_WORD_TOKENS[] = {
    LEADING_ANNOTATION,       DECLARATOR_ANNOTATION,      MACRO_INVOCATION_CALLEE,
    MACRO_TYPE_CALLEE,        CLASS_ANNOTATION_CALLEE,    CLASS_LEADING_ANNOTATION,
    FUNCTION_SUFFIX_ANNOTATION, JUXTAPOSED_CALLEE,        SECTION_LABEL,
    LEADING_ANNOTATION_CALLEE, MACRO_HEADER_CALLEE,       ENUMERATOR_ANNOTATION_CALLEE,
};

/// The spec limits delimiters to 16 chars
#define MAX_DELIMITER_LENGTH 16

typedef struct {
    uint8_t delimiter_length;
    wchar_t delimiter[MAX_DELIMITER_LENGTH];
    /// Conditionals taken in place whose `#endif` has not been consumed yet.
    uint8_t inline_conditionals;
} Scanner;

#include "scanner/lexing.h"
#include "scanner/words.h"
#include "scanner/balanced.h"
#include "scanner/conditionals.h"
#include "scanner/declarations.h"
#include "scanner/raw_strings.h"

/// Certify the identifier just read, whose end is marked, as an annotation or macro token.
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
        bool enumerator_annotation = valid_symbols[ENUMERATOR_ANNOTATION_CALLEE] &&
                                     !valid_symbols[EXPRESSION_START_PROBE];
        if (enumerator_annotation || valid_symbols[JUXTAPOSED_CALLEE]) {
            // After an enumerator's name or finished value, or after a call/number expression,
            // nothing ordinary begins with a call; share the balanced-argument scan so a mid-value
            // juxtaposition such as `0 ITEMS(ADD) + 1` still falls back without rewinding.
            if (!skip_parenthesized(lexer)) {
                return false;
            }
            if (enumerator_annotation && enumerator_annotation_tail_follows(lexer)) {
                lexer->result_symbol = ENUMERATOR_ANNOTATION_CALLEE;
                return true;
            }
            if (valid_symbols[JUXTAPOSED_CALLEE]) {
                lexer->result_symbol = JUXTAPOSED_CALLEE;
                return true;
            }
            return false;
        }
        if (valid_symbols[CLASS_ANNOTATION_CALLEE]) {
            lexer->result_symbol = CLASS_ANNOTATION_CALLEE;
            return class_annotation_follows(lexer);
        }
        if (!valid_symbols[MACRO_INVOCATION_CALLEE] && !valid_symbols[MACRO_TYPE_CALLEE] &&
            !valid_symbols[LEADING_ANNOTATION_CALLEE] && !valid_symbols[MACRO_HEADER_CALLEE]) {
            return false;
        }
        switch (classify_macro_call(lexer, valid_symbols[INITIALIZER_START_PROBE],
                                    valid_symbols[EXPRESSION_START_PROBE], valid_symbols[LEADING_ANNOTATION_CALLEE],
                                    valid_symbols[MACRO_INVOCATION_CALLEE])) {
            case LEADING_CALL:
                lexer->result_symbol = LEADING_ANNOTATION_CALLEE;
                return true;
            case MACRO_INVOCATION:
                lexer->result_symbol = MACRO_INVOCATION_CALLEE;
                return valid_symbols[MACRO_INVOCATION_CALLEE];
            case MACRO_TYPE:
                lexer->result_symbol = MACRO_TYPE_CALLEE;
                return valid_symbols[MACRO_TYPE_CALLEE];
            case MACRO_HEADER:
                lexer->result_symbol = MACRO_HEADER_CALLEE;
                return valid_symbols[MACRO_HEADER_CALLEE];
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
        bool class_key = false;
        if (leading_annotation_follows(lexer, !valid_symbols[MACRO_INVOCATION_CALLEE],
                                       valid_symbols[CLASS_KEY_MACRO] ? &class_key : NULL)) {
            return true;
        }
        lexer->result_symbol = CLASS_KEY_MACRO;
        return class_key;
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
    // A certified functional cast is zero-width before its `typename`.
    lexer->mark_end(lexer);
    if (!read_word(lexer, &word)) {
        return false;
    }
    if (word_is(&word, "typename") && valid_symbols[TYPENAME_CAST] && !valid_symbols[TEMPLATE_ARGUMENT_PROBE]) {
        // Where a declaration may start, `typename T::type(x);` declares `x`; only braces right
        // after the name, which no declarator precedes, make it a cast.
        lexer->result_symbol = TYPENAME_CAST;
        return dependent_cast_follows(lexer, valid_symbols[LEADING_ANNOTATION]);
    }
    if (word_in(&word, KEYWORDS)) {
        return false;
    }
    if (valid_symbols[MEMBER_POINTER_SCOPE] && lexer->lookahead == ':') {
        // Zero-width before the scope. A word followed by `:` is no other scanner token here.
        lexer->result_symbol = MEMBER_POINTER_SCOPE;
        return member_pointer_follows(lexer);
    }
    int32_t next = lexer->lookahead;
    bool plain = identifier && !word.non_ascii && next != '"' && next != '\'' && next != '\\';
    lexer->mark_end(lexer);
    if (valid_symbols[REF_NEW] && word_is(&word, "ref") && is_horizontal_space(next)) {
        // C++/CX `ref new T(...)`; any other `ref` is a name.
        skip_horizontal_space(lexer);
        Word after;
        lexer->result_symbol = read_word(lexer, &after) && word_is(&after, "new") ? REF_NEW : IDENTIFIER;
        return lexer->result_symbol == REF_NEW || plain;
    }
    if (certify && scan_certified_identifier(lexer, valid_symbols, newline_before)) {
        return true;
    }
    if (plain) {
        lexer->result_symbol = IDENTIFIER;
        return true;
    }
    return false;
}

void *tree_sitter_cpp_external_scanner_create() {
    Scanner *scanner = (Scanner *)ts_calloc(1, sizeof(Scanner));
    memset(scanner, 0, sizeof(Scanner));
    return scanner;
}

/// The balanced arguments after a certified leading annotation callee, kept opaque.
static bool scan_annotation_arguments(TSLexer *lexer) {
    while (is_space(lexer->lookahead)) {
        lexer->advance(lexer, true);
    }
    if (lexer->lookahead != '(' || !skip_parenthesized(lexer)) {
        return false;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = ANNOTATION_ARGUMENTS;
    return true;
}

/// From `(` with no space before it: a call's arguments, kept opaque, when `uncertain_arguments_follow`
/// certifies them. A function header continues with `{`, `:` or a suffix keyword: there these
/// arguments are parameters, and a parse stack reading a call must not outlive them. Where a
/// typeless declarator such as `A::A(int x)` may also start, only a statement's `;` certifies them.
static bool scan_uncertain_argument_list(TSLexer *lexer, const bool *valid_symbols) {
    if (!uncertain_arguments_follow(lexer)) {
        return false;
    }
    lexer->mark_end(lexer);
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    int32_t next = lexer->lookahead;
    if ((valid_symbols[PARAMETER_LIST_PROBE] && next != ';') || next == '{') {
        return false;
    }
    if (next == ':') {
        if (!skip_scope_operator(lexer)) {
            return false;
        }
    } else if (is_identifier_start(next)) {
        Word word;
        read_word(lexer, &word);
        if (word_in(&word, FUNCTION_SUFFIX_KEYWORDS)) {
            return false;
        }
    }
    lexer->result_symbol = UNCERTAIN_ARGUMENT_LIST;
    return true;
}

/// Zero-width end of a directive on the last line, which has no newline to end it.
static bool scan_preproc_end_of_input(TSLexer *lexer) {
    while (is_horizontal_space(lexer->lookahead) || lexer->lookahead == '\r') {
        lexer->advance(lexer, true);
    }
    if (!lexer->eof(lexer)) {
        return false;
    }
    lexer->result_symbol = PREPROC_END_OF_INPUT;
    return true;
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
    if (valid_symbols[ANNOTATION_ARGUMENTS]) {
        return scan_annotation_arguments(lexer);
    }
    if (lexer->lookahead == '(') {
        // Only the scans above start at `(`, so nothing else is lost when these arguments are not
        // certified. A space before `(` keeps the ordinary reading.
        return valid_symbols[UNCERTAIN_ARGUMENT_LIST] && scan_uncertain_argument_list(lexer, valid_symbols);
    }
    if (valid_symbols[PREPROC_END_OF_INPUT]) {
        // A directive line still needs its end: a newline, or end of input on the last line.
        return scan_preproc_end_of_input(lexer);
    }

    bool certify = false;
    for (size_t i = 0; i < sizeof CERTIFIED_WORD_TOKENS / sizeof *CERTIFIED_WORD_TOKENS && !certify; i++) {
        certify = valid_symbols[CERTIFIED_WORD_TOKENS[i]];
    }
    bool conditional_tokens = valid_symbols[PREPROC_INLINE_CONDITIONAL];
    if (!certify && !conditional_tokens && !valid_symbols[IDENTIFIER] && !valid_symbols[TYPENAME_CAST] &&
        !valid_symbols[MEMBER_POINTER_SCOPE]) {
        return false;
    }
    bool newline_before = false;
    bool space_before = false;
    unsigned skipped = 0;
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
        skipped++;
    }
    if (lexer->lookahead == '#') {
        // A directive begins its line. After a token on the same line, as inside the string
        // `"#ifdef X\n"`, a `#` never does; the column counts only the whitespace skipped here.
        if (!newline_before && lexer->get_column(lexer) != skipped) {
            return false;
        }
        return conditional_tokens && scan_preproc_conditional(scanner, lexer, valid_symbols);
    }
    // A literal suffix is immediate, so after a space only an identifier can match the word.
    bool identifier = valid_symbols[IDENTIFIER] && (space_before || !valid_symbols[LITERAL_SUFFIX]);
    if (!certify && !identifier && !valid_symbols[TYPENAME_CAST] && !valid_symbols[MEMBER_POINTER_SCOPE]) {
        return false;
    }
    return scan_identifier_token(lexer, valid_symbols, newline_before, certify, identifier);
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
