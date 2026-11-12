// Balanced bracket, block and template-argument scans, and argument-list classification.
// Part of scanner.c, which includes this file once after its token and state definitions.

#ifndef TREE_SITTER_CPP_SCANNER_BALANCED_H
#define TREE_SITTER_CPP_SCANNER_BALANCED_H

/// From `(`, consume the balanced argument list through its matching `)`. Strings, raw
/// strings, character literals, numbers and comments cannot unbalance the scan. Records whether
/// an argument holds what no parameter declaration can: a leading literal, or a member access
/// `.` before any default value, as in `M(T, Name, Changed.raise)`, and whether a top-level comma
/// separates several arguments.
static bool skip_balanced_list(TSLexer *lexer, bool *non_parameter, bool *several) {
    unsigned depth = 0;
    bool argument_start = false;
    bool default_value = false;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            argument_start = c == '(' && depth == 1;
            default_value = default_value && !argument_start;
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
            *several = *several || argument_start;
            default_value = default_value && !argument_start;
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            if (!skip_comment_after_slash(lexer, &newline)) {
                argument_start = false;
            }
        } else if (c == '"' || c == '\'') {
            *non_parameter = *non_parameter || argument_start;
            if (!skip_quoted(lexer)) {
                return false;
            }
            argument_start = false;
        } else if (is_digit(c)) {
            *non_parameter = *non_parameter || argument_start;
            skip_number(lexer);
            argument_start = false;
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (!skip_prefixed_literal(lexer, &word)) {
                return false;
            }
            argument_start = false;
        } else if (c == '#') {
            return false;
        } else if (c == '.' && depth == 1) {
            advance(lexer);
            if (lexer->lookahead == '.') {
                while (lexer->lookahead == '.') {
                    advance(lexer);
                }
            } else if (!default_value) {
                *non_parameter = true;
            }
            argument_start = false;
        } else {
            default_value = default_value || (c == '=' && depth == 1);
            advance(lexer);
            argument_start = false;
        }
    }
    return false;
}

/// As `skip_balanced_list`, without counting the arguments.
static bool skip_balanced_arguments(TSLexer *lexer, bool *non_parameter) {
    bool several = false;
    return skip_balanced_list(lexer, non_parameter, &several);
}

/// From `(`, consume a balanced argument list, whatever its arguments hold.
static bool skip_parenthesized(TSLexer *lexer) {
    bool non_parameter = false;
    return skip_balanced_arguments(lexer, &non_parameter);
}

/// From `{`, consume a balanced block, skipping literals and comments. Records whether the block
/// holds a `;` at its own depth, which only a block of statements or members does.
static bool skip_balanced_block(TSLexer *lexer, bool *statements) {
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
        } else if (c == ';') {
            *statements = *statements || depth == 1;
            advance(lexer);
        } else if (!skip_opaque_element(lexer)) {
            return false;
        }
    }
    return false;
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
        } else if (!skip_opaque_element(lexer)) {
            return false;
        }
    }
    return false;
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

/// From `(`, consume a call's balanced arguments through `)` when an argument holds two operands
/// side by side, which no expression or type argument can: two plain words, as in
/// `F(MACRO hr, x)` or `PRAGMA(clang diagnostic ignored "-W...")`, or a fundamental type and then
/// a name, as in `VERBOSE(const char *s = NULL)`. Keywords never count as plain words, a word after
/// `signed` or `unsigned` completes the type, as in `unsigned __int64`, words in template
/// arguments (`<` right after a word) never count, and once an argument holds a string literal its
/// words may be string macros being concatenated.
static bool uncertain_arguments_follow(TSLexer *lexer) {
    enum { OTHER, PLAIN_WORD, TYPE_WORD, SIGN_WORD } previous = OTHER;
    unsigned depth = 0;
    unsigned angles = 0;
    bool after_word = false;
    bool string_seen = false;
    bool juxtaposed = false;
    // A group `(...)` that is a whole argument's start, as in `M(a, (T name), b)`, is scanned like
    // an argument list of its own: `inner` holds its state at depth 2.
    bool argument_start = true;
    bool grouped = false;
    bool group_juxtaposed = false;
    int inner = OTHER;
    for (unsigned budget = MAX_INVOCATION_SCAN; budget > 0; --budget) {
        if (lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        bool word_before = after_word;
        after_word = false;
        bool starts_argument = argument_start && depth == 1;
        if (!is_space(c) && c != '/') {
            argument_start = false;
        }
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            grouped = depth == 2 ? c == '(' && starts_argument : grouped && depth > 2;
            group_juxtaposed = group_juxtaposed && depth > 2;
            argument_start = depth == 1;
            inner = OTHER;
            previous = OTHER;
            advance(lexer);
        } else if (c == ')' || c == ']' || c == '}') {
            advance(lexer);
            if (--depth == 0) {
                return c == ')' && juxtaposed;
            }
            if (depth == 1 && grouped) {
                while (is_space(lexer->lookahead)) {
                    advance(lexer);
                }
                juxtaposed = juxtaposed || (group_juxtaposed && (lexer->lookahead == ',' || lexer->lookahead == ')'));
            }
            grouped = grouped && depth >= 2;
            inner = OTHER;
            previous = OTHER;
        } else if (is_space(c)) {
            advance(lexer);
        } else if (c == '/') {
            bool newline = false;
            advance(lexer);
            if (!skip_comment_after_slash(lexer, &newline)) {
                previous = OTHER;
                inner = OTHER;
            }
        } else if (c == '"' || c == '\'') {
            if (!skip_quoted(lexer)) {
                return false;
            }
            string_seen = string_seen || (depth == 1 && c == '"');
            previous = OTHER;
            inner = OTHER;
        } else if (is_digit(c)) {
            skip_number(lexer);
            previous = OTHER;
            inner = OTHER;
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (lexer->lookahead == '"' && is_raw_prefix(&word)) {
                if (!skip_raw_string(lexer)) {
                    return false;
                }
                string_seen = string_seen || depth == 1;
                previous = OTHER;
                inner = OTHER;
            } else if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                bool string = lexer->lookahead == '"';
                if (!skip_quoted(lexer)) {
                    return false;
                }
                string_seen = string_seen || (depth == 1 && string);
                previous = OTHER;
                inner = OTHER;
            } else if (depth == 1 && angles == 0) {
                after_word = true;
                if (!word_in(&word, KEYWORDS)) {
                    if (previous == SIGN_WORD) {
                        previous = TYPE_WORD;
                    } else {
                        juxtaposed = juxtaposed || (previous != OTHER && !string_seen);
                        previous = PLAIN_WORD;
                    }
                } else if (word_is(&word, "signed") || word_is(&word, "unsigned")) {
                    previous = SIGN_WORD;
                } else if (is_fundamental_type_word(&word)) {
                    previous = TYPE_WORD;
                } else if (!((previous == TYPE_WORD || previous == SIGN_WORD) &&
                             is_cv_qualifier(&word))) {
                    previous = OTHER;
                }
            } else if (depth == 2 && grouped) {
                if (!word_in(&word, KEYWORDS)) {
                    group_juxtaposed = group_juxtaposed || inner == TYPE_WORD || inner == PLAIN_WORD;
                    inner = inner == SIGN_WORD ? TYPE_WORD : PLAIN_WORD;
                } else if (word_is(&word, "signed") || word_is(&word, "unsigned")) {
                    inner = SIGN_WORD;
                } else if (is_fundamental_type_word(&word)) {
                    inner = TYPE_WORD;
                } else if (!((inner == TYPE_WORD || inner == SIGN_WORD) &&
                             is_cv_qualifier(&word))) {
                    inner = OTHER;
                }
            } else {
                after_word = depth == 1;
            }
        } else if (c == '#') {
            return false;
        } else if (depth == 1 && c == '<' && word_before) {
            angles++;
            advance(lexer);
        } else if (depth == 1 && c == '>' && angles > 0) {
            // A closed template-id ends a name, so a word right after it is juxtaposed; `&&` or
            // `*` after it are operators, as in `is_same_v<A, B> && c`.
            angles--;
            previous = angles == 0 ? PLAIN_WORD : OTHER;
            advance(lexer);
        } else if (depth == 1 && c == '-') {
            advance(lexer);
            if (lexer->lookahead == '>') {
                advance(lexer);
            }
            previous = OTHER;
        } else if (depth == 1 && c == ',') {
            advance(lexer);
            string_seen = false;
            previous = OTHER;
            argument_start = true;
        } else if (depth == 2 && grouped && (inner == TYPE_WORD || inner == SIGN_WORD) &&
                   (c == '*' || c == '&')) {
            inner = TYPE_WORD;
            advance(lexer);
        } else if (depth == 1 && angles == 0 && (previous == TYPE_WORD || previous == SIGN_WORD) &&
                   (c == '*' || c == '&')) {
            previous = TYPE_WORD;
            advance(lexer);
        } else {
            advance(lexer);
            previous = OTHER;
            inner = OTHER;
        }
    }
    return false;
}

#endif // TREE_SITTER_CPP_SCANNER_BALANCED_H
