// Preprocessor directives: conditionals taken in place, and raw conditional arguments.
// Part of scanner.c, which includes this file once after its token and state definitions.

#ifndef TREE_SITTER_CPP_SCANNER_CONDITIONALS_H
#define TREE_SITTER_CPP_SCANNER_CONDITIONALS_H

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
    if (read_word(lexer, &word) && word_in(&word, CONDITIONAL_ALTERNATIVES)) {
        return false;
    }
    skip_directive_line(lexer);
    return true;
}

/// Conditional groups larger than this are left to the structured rules when those rules apply.
#define MAX_STRUCTURED_CONDITIONAL_SCAN 8192

typedef struct {
    bool complete;
    /// Some branch is unbalanced, starts by continuing an earlier construct, or ends with a
    /// statement header whose body follows the group.
    bool irregular;
    /// An `#else`/`#elif` exists; the token then extends through the last such line.
    bool alternative;
} ConditionalGroup;

/// After the group's `#endif`: true when the construct of the last branch continues there, with
/// `else` or a handler after a statement, or with `{` after a function header or `try`.
static bool construct_continues_after_group(TSLexer *lexer, bool open_function) {
    skip_directive_line(lexer);
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    if (lexer->lookahead == '{') {
        return open_function;
    }
    Word word;
    return read_word(lexer, &word) && (word_is(&word, "else") || word_is(&word, "catch") ||
                                       word_is(&word, "__except") || word_is(&word, "__finally"));
}

/// From the line after an opening `#if`, scan its group to the matching `#endif`. Nested groups
/// count only their first branch toward bracket balance, as a single preprocessing pass would.
/// A branch ending with `else`, `do` or a header such as `if (...)` leaves its statement's body
/// after the group, as in `if (a) x(); else` followed by `#endif` and a block.
static void scan_conditional_group(TSLexer *lexer, unsigned budget, ConditionalGroup *group) {
    unsigned nesting = 0;
    unsigned skipping = 0;
    int depth = 0;
    bool line_start = true;
    bool branch_start = true;
    // Header tracking: a header keyword was just read, the depth its `(` opened at, and whether
    // the branch so far ends with a header that still needs a body.
    bool header_keyword = false;
    int header_depth = -1;
    bool open_header = false;
    // Function-header tracking at depth 0: words of the current item before its first `(`, whether
    // the branch so far ends with `name(...)` and suffix words or with `try`, and whether some
    // branch ended so. A header needs a declared type or specifier before its name, so a call
    // statement such as `LOG()` never counts.
    unsigned header_words = 0;
    bool parameters_seen = false;
    bool after_operator = false;
    bool open_function = false;
    bool open_function_branch = false;
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
            if (word_in(&word, CONDITIONAL_OPENINGS)) {
                nesting++;
            } else if (word_is(&word, "endif")) {
                if (nesting == 0) {
                    group->irregular = group->irregular || depth != 0 || open_header;
                    group->complete = true;
                    open_function_branch = open_function_branch || open_function;
                    group->irregular =
                        group->irregular || construct_continues_after_group(lexer, open_function_branch);
                    return;
                }
                if (skipping == nesting) {
                    skipping = 0;
                }
                nesting--;
            } else if (word_in(&word, CONDITIONAL_ALTERNATIVES)) {
                if (nesting == 0) {
                    group->irregular = group->irregular || depth != 0 || open_header;
                    group->alternative = true;
                    open_function_branch = open_function_branch || open_function;
                    header_words = 0;
                    parameters_seen = false;
                    open_function = false;
                    depth = 0;
                    header_keyword = false;
                    header_depth = -1;
                    open_header = false;
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
        bool keyword = header_keyword;
        header_keyword = false;
        if (counted) {
            open_header = false;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (counted && keyword && c == '(' && depth == 0) {
                header_depth = depth;
            }
            if (counted && depth == 0) {
                if (c == '{') {
                    header_words = 0;
                    parameters_seen = false;
                } else if (c == '(' && !parameters_seen) {
                    parameters_seen = header_words >= 2;
                }
                after_operator = false;
                open_function = false;
            }
            depth += counted;
            advance(lexer);
        } else if (c == ')' || c == ']' || c == '}') {
            depth -= counted;
            if (depth < 0 && nesting == 0) {
                group->irregular = true;
            }
            if (counted && c == ')' && depth == header_depth) {
                open_header = true;
                header_depth = -1;
            }
            if (counted && depth == 0) {
                open_function = c == ')' && parameters_seen;
                if (c == '}') {
                    header_words = 0;
                    parameters_seen = false;
                }
            }
            advance(lexer);
        } else if (c == '"' || c == '\'') {
            skip_quoted(lexer);
        } else if (is_digit(c)) {
            skip_number(lexer);
        } else if (is_identifier_start(c)) {
            Word word;
            read_word(lexer, &word);
            if (first && (word_is(&word, "else") || word_is(&word, "catch"))) {
                group->irregular = true;
            }
            if (counted && depth == 0) {
                header_keyword = word_in(&word, HEADER_KEYWORDS) || (keyword && word_is(&word, "constexpr"));
                open_header = word_is(&word, "else") || word_is(&word, "do");
                if (word_is(&word, "try") || word_is(&word, "__try")) {
                    open_function = true;
                } else if (parameters_seen) {
                    // Suffixes such as `const`, `noexcept` or an annotation macro keep it open.
                    open_function = open_function && !word_in(&word, HEADER_KEYWORDS) && !word_is(&word, "return");
                } else {
                    header_words++;
                    after_operator = word_is(&word, "operator");
                    open_function = false;
                }
            }
            skip_prefixed_literal(lexer, &word);
        } else {
            if (counted && depth == 0) {
                open_function = false;
                if (c == ';') {
                    header_words = 0;
                    parameters_seen = false;
                } else if (!parameters_seen && !after_operator && c != ':' && c != '<' && c != '>' && c != '*' &&
                           c != '&' && c != '~' && c != ',') {
                    // `=` or another operator before `(` makes the item an expression; an operator
                    // function's name, as in `operator<=>`, does not.
                    header_words = 0;
                }
            }
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
    if (!word_in(&word, CONDITIONAL_OPENINGS)) {
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
            if (!skip_line_continuation(lexer)) {
                return false;
            }
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

#endif // TREE_SITTER_CPP_SCANNER_CONDITIONALS_H
