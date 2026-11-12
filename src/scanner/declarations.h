// Certification of annotation and macro shapes in declarations, by what follows them.
// Part of scanner.c, which includes this file once after its token and state definitions.

#ifndef TREE_SITTER_CPP_SCANNER_DECLARATIONS_H
#define TREE_SITTER_CPP_SCANNER_DECLARATIONS_H

/// Declarator suffix words read between a constructor's parameters and its initializer list.
#define MAX_SUFFIX_WORDS 4

/// CONSTRUCTOR_HEAD: `B(...) [suffixes] : init`, no macro but a constructor declarator.
/// MACRO_HEADER: `M(...)(params) [suffixes];`, a macro producing a function header.
typedef enum { NOT_MACRO, MACRO_INVOCATION, MACRO_TYPE, LEADING_CALL, CONSTRUCTOR_HEAD, MACRO_HEADER } MacroShape;

// Mutually recursive certifications.
static bool type_declarator_follows(TSLexer *lexer, bool qualified);
static bool annotation_tail_follows(TSLexer *lexer, bool after_specifier, bool mid_declaration, bool *class_key);
static bool annotation_chain_follows(TSLexer *lexer);
static bool constructor_or_annotation_call_follows(TSLexer *lexer);
static bool constructor_initializers_follow(TSLexer *lexer);
static bool call_annotation_keyword_follows(TSLexer *lexer, const Word *word);
static bool macro_header_follows(TSLexer *lexer, bool expression_possible);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Leading annotations: words and calls before a declaration's type.

/// After `__declspec` (consumed): its balanced arguments.
static bool skip_declspec_arguments(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    return lexer->lookahead == '(' && skip_parenthesized(lexer);
}

/// After `operator` (consumed): a conversion function's type name follows, as in `operator T()`,
/// which no return type can precede. `operator new`, `operator delete` and `operator co_await`
/// are ordinary operator functions.
static bool conversion_type_follows(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    Word word;
    return read_word(lexer, &word) && !word_is(&word, "new") && !word_is(&word, "delete") &&
           !word_is(&word, "co_await");
}

/// After a class name's `:`: an access specifier or `virtual` begins a base clause.
static bool base_clause_follows(TSLexer *lexer) {
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    Word word;
    return read_word(lexer, &word) && (word_is(&word, "public") || word_is(&word, "private") ||
                                       word_is(&word, "protected") || word_is(&word, "virtual"));
}

/// After a word on its own line: true when a keyword type follows, possibly after specifiers.
/// `auto` counts only after a specifier, because `A auto` is a constrained placeholder, and so
/// does `namespace`, as in `inline namespace`.
/// Reports whether specifiers were consumed before the word left in `word`.
static bool keyword_declaration_follows(TSLexer *lexer, Word *word, bool *specified) {
    bool after_specifier = false;
    for (;;) {
        if (word_in(word, TYPE_KEYWORDS) ||
            (after_specifier && (is_placeholder_keyword(word) || word_is(word, "namespace")))) {
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

/// After a type name's first word (consumed): consume its `::` qualifiers and template arguments,
/// then certify that a declarator follows: cv-qualifiers, calling conventions, `*` and `&`, then
/// a name. A `(` right after the type is left unclaimed, since `A ns::f(` and `A f<T>(` are
/// ordinary declarators of type `A`. Only a qualified or templated type, which a declarator
/// cannot continue, may put its declarator on a later line.
static bool type_declarator_follows(TSLexer *lexer, bool qualified) {
    Word word;
    bool scoped = false;
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '<') {
            if (!skip_template_arguments(lexer)) {
                return false;
            }
            qualified = true;
        } else if (lexer->lookahead == ':') {
            if (!skip_scope_operator(lexer)) {
                return false;
            }
            skip_horizontal_space(lexer);
            if (!read_word(lexer, &word) || !can_name_type(&word)) {
                return false;
            }
            qualified = true;
            scoped = true;
        } else {
            break;
        }
    }
    bool pointer = false;
    for (;;) {
        bool newline = false;
        if (!skip_trivia(lexer, &newline) || (newline && !qualified)) {
            return false;
        }
        if (lexer->lookahead == '*' || lexer->lookahead == '&' || lexer->lookahead == '^') {
            // `^` declares a C++/CX handle like a pointer.
            advance(lexer);
            pointer = true;
            continue;
        }
        if (lexer->lookahead == '(') {
            // `T& (name)(...)` or `T (*f)(...)`: after a pointer operator, `(` begins a declarator.
            // `A<T>::A(...) : x(y)` names an out-of-line constructor with its initializers.
            return pointer || (scoped && constructor_initializers_follow(lexer));
        }
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (is_cv_qualifier(&word) || word_in(&word, CALLING_CONVENTIONS)) {
            continue;
        }
        // A keyword type after the scanned words, as in `A(x) _In_ size_t n`, shows they were all
        // annotations.
        return !word_in(&word, KEYWORDS) || word_in(&word, TYPE_KEYWORDS) || word_is(&word, "operator") ||
               is_contextual_name(&word);
    }
}

/// After a scope's `::` (consumed): a scoped type name, then a declarator, as `type_declarator_follows`
/// certifies.
static bool scoped_type_declarator_follows(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    Word scope;
    return read_word(lexer, &scope) && can_name_type(&scope) && type_declarator_follows(lexer, true);
}

/// After `A` (already consumed): certify `A [B ...] [specifier keywords]` followed on one physical
/// line by a keyword type, by a qualified or templated type and its declarator, or, after a
/// specifier keyword, by any type name and its declarator. `A B` alone stays a type and its
/// declarator, so a plain type name directly after annotations is never claimed.
static bool leading_annotation_follows(TSLexer *lexer, bool mid_declaration, bool *class_key) {
    if (lexer->lookahead == ':' || lexer->lookahead == '<') {
        // `A::` and `A<` begin a qualified or templated name.
        return false;
    }
    return annotation_tail_follows(lexer, false, mid_declaration, class_key);
}

/// After leading annotations (consumed), and after a specifier keyword when `after_specifier`:
/// the rest of a certified annotation sequence, as described for `leading_annotation_follows`.
/// A destructor's `~` also ends one, as does a plain word followed by a cv-qualifier, which is a
/// type: `_In_ T const&` has no reading with `_In_` as the type. Annotations filling a line may
/// continue on the next one only when it begins with a keyword type or specifier, as in
/// `NO_RETURN NEVER_INLINE` above `static void f()`. A line holding a single annotation continues
/// only `mid_declaration`, after earlier annotations; at a statement start it is a bare macro.
/// With `class_key`, also recognizes `A __declspec(...) Name : public B` and `A __declspec(...) Name {`
/// with member declarations: `A` stands for a class key, so `*class_key` is set and false returned.
static bool annotation_tail_follows(TSLexer *lexer, bool after_specifier, bool mid_declaration, bool *class_key) {
    Word word;
    bool plain_before = false;
    bool words = mid_declaration;
    unsigned declspecs = 0;
    bool other_words = false;
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':') {
            return type_declarator_follows(lexer, true);
        }
        if (lexer->lookahead == '~') {
            return true;
        }
        if (words && !after_specifier && (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
            while (is_space(lexer->lookahead)) {
                advance(lexer);
            }
            if (!read_word(lexer, &word)) {
                return false;
            }
            if (word_in(&word, TYPE_KEYWORDS)) {
                return true;
            }
            if (!word_in(&word, KEYWORDS)) {
                // `MACRO` alone on its line, then `T f(...)`: a type and its declarator, or
                // another annotation call continuing the chain.
                skip_horizontal_space(lexer);
                if (lexer->lookahead == '(') {
                    return constructor_or_annotation_call_follows(lexer);
                }
                return type_declarator_follows(lexer, false);
            }
            if (word_is(&word, "__declspec") && !skip_declspec_arguments(lexer)) {
                return false;
            }
            if (!word_is(&word, "__declspec") && !word_in(&word, SPECIFIER_KEYWORDS)) {
                return false;
            }
            after_specifier = true;
            plain_before = false;
            continue;
        }
        if (!read_word(lexer, &word)) {
            return false;
        }
        words = true;
        bool first_other = !other_words && !word_is(&word, "__declspec");
        other_words = other_words || first_other;
        if (plain_before && is_cv_qualifier(&word)) {
            return true;
        }
        plain_before = false;
        if (word_in(&word, TYPE_KEYWORDS) || (after_specifier && is_placeholder_keyword(&word))) {
            return true;
        }
        if (word_in(&word, SPECIFIER_KEYWORDS)) {
            after_specifier = true;
            continue;
        }
        if (word_is(&word, "decltype")) {
            // `_In_opt_ decltype(x) name`: no type precedes `decltype`, but a concept precedes the
            // placeholder `decltype(auto)`.
            skip_horizontal_space(lexer);
            if (lexer->lookahead != '(') {
                return false;
            }
            advance(lexer);
            skip_horizontal_space(lexer);
            Word inner;
            return !read_word(lexer, &inner) || !word_is(&inner, "auto");
        }
        if (word_is(&word, "__declspec")) {
            // `A __declspec(noinline) inline T f()`: like a specifier.
            if (!skip_declspec_arguments(lexer)) {
                return false;
            }
            declspecs++;
            after_specifier = true;
            continue;
        }
        if (word_in(&word, KEYWORDS)) {
            // `A explicit operator bool()` and `A operator T()` declare conversion functions.
            return word_is(&word, "operator") && (after_specifier || conversion_type_follows(lexer));
        }
        if (after_specifier) {
            skip_horizontal_space(lexer);
            if (lexer->lookahead == '(') {
                return constructor_or_annotation_call_follows(lexer);
            }
            if (class_key != NULL && declspecs > 0 && first_other) {
                // `A __declspec(uuid(...)) Name : public B` or `... Name {` with members: no type is
                // followed by its declarator and then these, so `A` stands for a class key.
                if (lexer->lookahead == '{') {
                    *class_key = class_body_members_follow(lexer);
                    return false;
                }
                if (lexer->lookahead == ':') {
                    advance(lexer);
                    if (lexer->lookahead != ':') {
                        *class_key = base_clause_follows(lexer);
                        return false;
                    }
                    // `A __declspec(x) T::U name`: the type continues past its scope.
                    advance(lexer);
                    return scoped_type_declarator_follows(lexer);
                }
            }
            if (!is_identifier_start(lexer->lookahead)) {
                // After a specifier the word is the type, whose declarator may start the next line.
                return type_declarator_follows(lexer, lexer->lookahead == '\n' || lexer->lookahead == '\r');
            }
            // `A inline B void f()`: a keyword type or specifier after `B` makes `B` an annotation
            // too; a cv-qualifier or calling convention continues `B` as the type.
            Word next;
            read_word(lexer, &next);
            if (word_in(&next, TYPE_KEYWORDS)) {
                return true;
            }
            if (is_cv_qualifier(&next) || word_in(&next, CALLING_CONVENTIONS)) {
                return type_declarator_follows(lexer, false);
            }
            if (word_in(&next, SPECIFIER_KEYWORDS)) {
                continue;
            }
            return !word_in(&next, KEYWORDS) || word_is(&next, "operator");
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':' || lexer->lookahead == '<') {
            return type_declarator_follows(lexer, true);
        }
        if (lexer->lookahead == '(') {
            return constructor_or_annotation_call_follows(lexer);
        }
        plain_before = true;
    }
}

/// After an annotation call's arguments: more annotations, then constructor initializers, a keyword
/// type, or a type and its declarator.
static bool annotation_chain_follows(TSLexer *lexer) {
    bool newline = false;
    bool after_call = true;
    Word word;
    for (unsigned words = 0; words <= MAX_SUFFIX_WORDS; words++) {
        if (!skip_trivia(lexer, &newline)) {
            return false;
        }
        if (lexer->lookahead == '(' && after_call) {
            // `M(R, Name)(params)`: the last call produced a function header.
            return macro_header_follows(lexer, false);
        }
        after_call = false;
        if (lexer->lookahead == ':') {
            advance(lexer);
            return lexer->lookahead != ':';
        }
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (word_in(&word, TYPE_KEYWORDS)) {
            return true;
        }
        bool keyword = word_in(&word, KEYWORDS);
        if (keyword && !word_in(&word, DECLARATOR_SUFFIX_WORDS)) {
            return false;
        }
        skip_horizontal_space(lexer);
        int32_t c = lexer->lookahead;
        if (c == '(') {
            if (!skip_parenthesized(lexer)) {
                return false;
            }
            after_call = true;
            continue;
        }
        if (keyword || c == '\n' || c == '\r' || c == '/') {
            // A suffix such as `noexcept`, or an annotation ending its line: more follows.
            continue;
        }
        if (c == ':') {
            advance(lexer);
            if (lexer->lookahead != ':') {
                // `B(params) WI_NOEXCEPT : init`: a suffix macro, then the initializers.
                return true;
            }
            advance(lexer);
            return scoped_type_declarator_follows(lexer);
        }
        return (is_identifier_start(c) || c == '*' || c == '&' || c == '<') && type_declarator_follows(lexer, c == '<');
    }
    return false;
}

/// After a word directly followed by `(` in an annotation tail: `B(params) suffixes : init` is a
/// constructor, and `B(args)` an annotation call when, after further annotations, possibly on the
/// next lines, a keyword type or a type and its declarator follow, as in `_In_ _In_range_(1, N)
/// size_t n` or `inline _IRQL_requires_max_(L)` / `_Acquires_lock_(l)` / `T f(...)`.
static bool constructor_or_annotation_call_follows(TSLexer *lexer) {
    return skip_parenthesized(lexer) && annotation_chain_follows(lexer);
}

/// After `A(...) K` with keyword `K` consumed: `A(...)` annotates a declaration when `K` is a
/// keyword type, or a specifier or `__declspec(...)` that a certified annotation tail follows, as
/// in `_Always_(...) __declspec(noinline) inline NTSTATUS f(`.
static bool call_annotation_keyword_follows(TSLexer *lexer, const Word *word) {
    if (word_in(word, TYPE_KEYWORDS)) {
        return true;
    }
    if (word_is(word, "__declspec")) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead != '(' || !skip_parenthesized(lexer)) {
            return false;
        }
        return annotation_tail_follows(lexer, true, false, NULL);
    }
    return word_in(word, SPECIFIER_KEYWORDS) && annotation_tail_follows(lexer, true, false, NULL);
}

/// After `A(...) B` and a line break where no statement may start: the declaration goes on, with
/// `B` its type above the declarator, as in `T` above `name(...)`, or with `B` one more annotation
/// above `__declspec(x) inline T f()` or a type and its declarator.
static bool declaration_line_follows(TSLexer *lexer) {
    bool newline = false;
    Word word;
    if (!skip_trivia(lexer, &newline) || !read_word(lexer, &word)) {
        return false;
    }
    if (word_in(&word, KEYWORDS)) {
        return word_is(&word, "operator") || call_annotation_keyword_follows(lexer, &word);
    }
    skip_horizontal_space(lexer);
    int32_t c = lexer->lookahead;
    return c == '(' || c == ';' || c == '=' || c == '[' || type_declarator_follows(lexer, c == ':' || c == '<');
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Macro calls: statement-like invocations, macro types, macro-produced headers and constructors.

/// After `M(...)` (consumed) at `(`: a function header produced by a macro, as in
/// `STDMETHOD_(void, Run)() override;` or `M(T, Name)(int x) SUFFIX {`: balanced parameters, then
/// declarator suffixes and an ending only a declaration has: a body, `= 0`, `= default`, `= delete`
/// or `;`. Where an expression may start, `f(x)(y);` and `f(x)(y) = 0;` stay calls unless a suffix
/// word shows the declaration.
static bool macro_header_follows(TSLexer *lexer, bool expression_possible) {
    bool non_parameter = false;
    bool newline = false;
    if (!skip_balanced_arguments(lexer, &non_parameter) || non_parameter) {
        return false;
    }
    bool suffix = false;
    Word word;
    for (unsigned words = 0;; words++) {
        if (!skip_trivia(lexer, &newline)) {
            return false;
        }
        if (!is_identifier_start(lexer->lookahead)) {
            break;
        }
        if (words == MAX_SUFFIX_WORDS || !read_word(lexer, &word) ||
            (word_in(&word, KEYWORDS) && !word_in(&word, FUNCTION_SUFFIX_KEYWORDS))) {
            return false;
        }
        suffix = true;
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '(' && !skip_parenthesized(lexer)) {
            return false;
        }
    }
    switch (lexer->lookahead) {
        case '{':
            return true;
        case ';':
            return suffix || !expression_possible;
        case '=':
            if (expression_possible && !suffix) {
                return false;
            }
            advance(lexer);
            if (!skip_trivia(lexer, &newline)) {
                return false;
            }
            if (lexer->lookahead == '0') {
                return true;
            }
            return read_word(lexer, &word) && (word_is(&word, "default") || word_is(&word, "delete"));
        default:
            return false;
    }
}

/// At the `(` after a word: `M(a, b)(params) suffixes;`, a macro header as `macro_header_follows`
/// describes, which may follow annotations, or another annotation call in a chain of them.
static bool macro_header_call_follows(TSLexer *lexer) {
    bool non_parameter = false;
    bool several = false;
    bool newline = false;
    if (!skip_balanced_list(lexer, &non_parameter, &several) || !skip_trivia(lexer, &newline)) {
        return false;
    }
    if (lexer->lookahead == '(') {
        return several && !non_parameter && macro_header_follows(lexer, false);
    }
    return annotation_chain_follows(lexer);
}

/// After `C(...) A` (consumed), with optional arguments of `A` next: true when a member
/// initializer list, `= default`/`= delete` or a body follows, which ends a constructor declarator
/// with a suffix annotation `A` and never the declarator `A` of a function-like macro type `C(...)`.
/// Sets `initializers` when a member initializer list follows. With `annotation`, after `A(...) B(...)`
/// instead sets it when a declaration follows, as in `_Post_(x) _When_(y, z) int f();`, which makes
/// both calls annotations.
static bool constructor_annotation_follows(TSLexer *lexer, bool *initializers, bool *annotation) {
    skip_horizontal_space(lexer);
    bool header = false;
    if (lexer->lookahead == '(') {
        bool non_parameter = false;
        if (!skip_balanced_list(lexer, &non_parameter, &header)) {
            return false;
        }
        header = header && !non_parameter;
    }
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    int32_t c = lexer->lookahead;
    if (annotation != NULL && c == '(' && header) {
        // `A(...) M(T, Name)(params) suffixes;`: `M` produces the header after annotation `A`.
        *annotation = macro_header_follows(lexer, false);
        return false;
    }
    if (annotation != NULL && is_identifier_start(c)) {
        Word word;
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (word_in(&word, KEYWORDS)) {
            *annotation = call_annotation_keyword_follows(lexer, &word);
        } else {
            skip_horizontal_space(lexer);
            *annotation = lexer->lookahead == '(' ? macro_header_call_follows(lexer)
                                                  : type_declarator_follows(lexer, lexer->lookahead == ':' ||
                                                                                       lexer->lookahead == '<');
        }
        return false;
    }
    if (c == '{') {
        // A body of statements, or `{}` not ending a declaration, follows a constructor; a braced
        // initializer of a macro type's declarator holds no statements and ends with `;` or `,`.
        bool statements = false;
        if (!skip_balanced_block(lexer, &statements)) {
            return false;
        }
        if (statements) {
            return true;
        }
        if (!skip_trivia(lexer, &newline)) {
            return false;
        }
        c = lexer->lookahead;
        return !lexer->eof(lexer) && c != ';' && c != ',' && c != ')';
    }
    if (c != ':' && c != '=') {
        return false;
    }
    advance(lexer);
    if (c == ':') {
        *initializers = lexer->lookahead != ':';
        return *initializers;
    }
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    Word word;
    return read_word(lexer, &word) && (word_is(&word, "default") || word_is(&word, "delete"));
}

/// At the `(` after a name that follows annotations: true when balanced parentheses are followed by
/// a member initializer list's single `:`, which only a constructor declarator can end with, as in
/// `FORCE_INLINE Ref(T *p) : ptr(p) {}`. A body alone stays unclaimed: `A B(...) {}` may still be a
/// function `B` returning `A`.
static bool constructor_initializers_follow(TSLexer *lexer) {
    bool newline = false;
    if (!skip_parenthesized(lexer) || !skip_trivia(lexer, &newline)) {
        return false;
    }
    // Declarator suffixes may come first: `noexcept(...)`, `throw()` or a macro such as `WI_NOEXCEPT`.
    Word word;
    for (unsigned suffixes = 0; is_identifier_start(lexer->lookahead); suffixes++) {
        if (suffixes == MAX_SUFFIX_WORDS || !read_word(lexer, &word) ||
            (word_in(&word, KEYWORDS) && !word_in(&word, DECLARATOR_SUFFIX_WORDS))) {
            return false;
        }
        if (!skip_trivia(lexer, &newline) ||
            (lexer->lookahead == '(' && (!skip_parenthesized(lexer) ||
                                         !skip_trivia(lexer, &newline)))) {
            return false;
        }
    }
    if (lexer->lookahead != ':') {
        return false;
    }
    advance(lexer);
    return lexer->lookahead != ':';
}

/// After callee `A` (consumed) and `(`: classify `A(...)` by what follows its balanced argument
/// list. It is a statement-like invocation when nothing can continue an expression or
/// declarator, and a type when a declarator follows on the same line. Where a declaration's
/// specifiers may start, it is a leading annotation call when a keyword type, a specifier, or a
/// type name and its declarator follow on the same line, which a macro type never can.
static MacroShape classify_macro_call(TSLexer *lexer, bool initializer_possible, bool expression_possible,
                                      bool annotation_possible, bool invocation_possible) {
    bool non_parameter = false;
    bool several = false;
    if (!skip_balanced_list(lexer, &non_parameter, &several)) {
        return NOT_MACRO;
    }
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return NOT_MACRO;
    }
    if (lexer->eof(lexer)) {
        return MACRO_INVOCATION;
    }
    // Where no statement-like invocation may stand, as after `template <...>` or a specifier, a
    // leading annotation call may end its line: `inline _IRQL_requires_(X)` above `void f()`.
    bool relaxed = newline && annotation_possible && !invocation_possible;
    if (relaxed) {
        newline = false;
    }
    int32_t c = lexer->lookahead;
    if (c == '}') {
        return !initializer_possible || !braces_close_initializer(lexer) ? MACRO_INVOCATION : NOT_MACRO;
    }
    // No parameter holds a leading literal or a member access, so `A("x") {` and member-level
    // `A(T, 0);` or `A(T, x.y);` cannot be constructor declarations.
    if (c == '{') {
        return non_parameter ? MACRO_INVOCATION : NOT_MACRO;
    }
    if (c == ';') {
        return non_parameter && !expression_possible ? MACRO_INVOCATION : NOT_MACRO;
    }
    Word word;
    bool initializers = false;
    if (!newline) {
        if (c == '(') {
            // Several arguments: `T (*name)(...)` and `T (name)(...)` hold a single declarator.
            return several && !non_parameter && macro_header_follows(lexer, expression_possible) ? MACRO_HEADER
                                                                                                 : NOT_MACRO;
        }
        if (c == ':') {
            advance(lexer);
            return lexer->lookahead == ':' ? NOT_MACRO : CONSTRUCTOR_HEAD;
        }
        // `A(...) name` is never an expression; `A(...) *name` is one only where expressions may
        // start.
        if ((c == '*' || c == '&') && !expression_possible) {
            return MACRO_TYPE;
        }
        if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
            return NOT_MACRO;
        }
        if (word_in(&word, KEYWORDS)) {
            if (!annotation_possible && word_in(&word, DECLARATOR_SUFFIX_WORDS)) {
                return constructor_annotation_follows(lexer, &initializers, NULL) && initializers ? CONSTRUCTOR_HEAD
                                                                                            : NOT_MACRO;
            }
            return annotation_possible && call_annotation_keyword_follows(lexer, &word) ? LEADING_CALL : NOT_MACRO;
        }
        if (!annotation_possible) {
            if (!constructor_annotation_follows(lexer, &initializers, NULL)) {
                return MACRO_TYPE;
            }
            return initializers ? CONSTRUCTOR_HEAD : NOT_MACRO;
        }
        skip_horizontal_space(lexer);
        c = lexer->lookahead;
        if (c == ':') {
            // `C(...) A : init` is a constructor; `A(...) ns::T name` a qualified type after a call.
            if (!skip_scope_operator(lexer)) {
                return NOT_MACRO;
            }
            return scoped_type_declarator_follows(lexer) ? LEADING_CALL : MACRO_TYPE;
        }
        if (relaxed && (c == '\n' || c == '\r')) {
            return declaration_line_follows(lexer) ? LEADING_CALL : MACRO_TYPE;
        }
        if (c == '(' || c == '=' || c == '\n' || c == '\r' || c == '/') {
            bool chained = false;
            if (constructor_annotation_follows(lexer, &initializers, c == '(' ? &chained : NULL)) {
                return NOT_MACRO;
            }
            return chained ? LEADING_CALL : MACRO_TYPE;
        }
        return type_declarator_follows(lexer, false) ? LEADING_CALL : MACRO_TYPE;
    }
    if (c == '#' || c == '~') {
        return MACRO_INVOCATION;
    }
    if (c == '[' || c == ':') {
        advance(lexer);
        if (lexer->lookahead == c) {
            return MACRO_INVOCATION;
        }
        return c == ':' ? CONSTRUCTOR_HEAD : NOT_MACRO;
    }
    if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
        return NOT_MACRO;
    }
    if (word_in(&word, CONTINUATION_WORDS) || (!expression_possible && word_in(&word, DECLARATOR_SUFFIX_WORDS))) {
        return NOT_MACRO;
    }
    return MACRO_INVOCATION;
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

/// After bare `A` (consumed) followed by a line break or `}`: certify a statement-like macro
/// only when `A` cannot begin or end any ordinary expression or declaration.
static bool bare_macro_follows(TSLexer *lexer, bool initializer_possible) {
    bool newline = false;
    if (!skip_trivia(lexer, &newline)) {
        return false;
    }
    while (newline && lexer->lookahead == '#') {
        // A directive other than a conditional's opening line cannot fall inside a declaration
        // `A ...` that started on the line above, as `#endif` after `IM_MSVC_RUNTIME_CHECKS_RESTORE`.
        advance(lexer);
        skip_horizontal_space(lexer);
        Word directive;
        if (!read_word(lexer, &directive)) {
            return false;
        }
        if (word_in(&directive, LINE_ENDING_DIRECTIVES)) {
            return true;
        }
        if (!word_in(&directive, CONDITIONAL_OPENINGS)) {
            return false;
        }
        skip_directive_line(lexer);
        if (!skip_trivia(lexer, &newline)) {
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
        // possible braced initializer it may be an element. A block holding statements is
        // neither, so `A` takes it even before `;`, as in folly's `SCOPE_EXIT { f(); };`.
        bool after_block = false;
        bool statements = false;
        if (!skip_balanced_block(lexer, &statements) || !skip_trivia(lexer, &after_block)) {
            return false;
        }
        if (statements && lexer->lookahead == ';') {
            return true;
        }
        return (statements || !initializer_possible) && after_block &&
               (lexer->lookahead == '#' || new_item_starts(lexer));
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
    if (word_is(&word, "operator")) {
        return conversion_type_follows(lexer);
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
        // A member initializer list after `B(...)` ends only a constructor declarator. After a
        // specifier, `B(...)` may also annotate the declaration that follows it.
        if (specified) {
            return constructor_or_annotation_call_follows(lexer);
        }
        MacroShape shape = classify_macro_call(lexer, false, true, false, true);
        return shape == CONSTRUCTOR_HEAD || shape == MACRO_INVOCATION;
    }
    for (;;) {
        skip_horizontal_space(lexer);
        if (lexer->lookahead == ':') {
            if (!skip_scope_operator(lexer)) {
                return false;
            }
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
    // `A` and `B` on lines of their own before directives that keep the path, as in `END_NS`,
    // `#if X`, `DIAGNOSTIC_POP`, `#endif` at the end of a header.
    while (lexer->lookahead == '#') {
        if (!skip_path_directive(lexer) || !skip_trivia(lexer, &after_second)) {
            return false;
        }
    }
    return lexer->eof(lexer) || new_item_starts(lexer);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Declarator annotations: after a declarator's name, a function's parameters or an enumerator.

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
    return !word_in(&word, KEYWORDS) || strcmp(word.text, "operator") == 0 || is_contextual_name(&word);
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
            if (!skip_parenthesized(lexer)) {
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

/// After an enumerator annotation call's balanced arguments: certify when further annotation
/// calls or attributes end at `=`, `,`, `}` or a conditional directive line inside the enum.
static bool enumerator_annotation_tail_follows(TSLexer *lexer) {
    for (unsigned count = 0; count < MAX_FUNCTION_SUFFIX_ANNOTATIONS; count++) {
        bool newline = false;
        if (!skip_trivia(lexer, &newline) || lexer->eof(lexer)) {
            return false;
        }
        int32_t c = lexer->lookahead;
        if (c == ',' || c == '}' || (newline && c == '#')) {
            return true;
        }
        if (c == '=') {
            advance(lexer);
            return lexer->lookahead != '=';
        }
        if (c == '[') {
            advance(lexer);
            if (lexer->lookahead != '[' || !skip_parenthesized(lexer) ||
                lexer->lookahead != ']') {
                return false;
            }
            advance(lexer);
            continue;
        }
        Word word;
        if (!read_word(lexer, &word) || lexer->lookahead == '"' || lexer->lookahead == '\'') {
            return false;
        }
        if (word_is(&word, "__attribute__") || word_is(&word, "__attribute")) {
            skip_horizontal_space(lexer);
            if (lexer->lookahead != '(' || !skip_parenthesized(lexer)) {
                return false;
            }
            continue;
        }
        if (word_in(&word, KEYWORDS)) {
            return false;
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead != '(' || !skip_parenthesized(lexer)) {
            return false;
        }
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Classes: annotations in class heads, and section labels among members.

/// After a class key and callee `A` (consumed) at `(`: `A(...) Name` is an annotation call before
/// the class name, since an elaborated type's declarator is never followed by a name.
static bool class_annotation_follows(TSLexer *lexer) {
    bool newline = false;
    if (!skip_parenthesized(lexer) || !skip_trivia(lexer, &newline)) {
        return false;
    }
    Word word;
    return read_word(lexer, &word) && !word_in(&word, KEYWORDS) && lexer->lookahead != '"' && lexer->lookahead != '\'';
}

/// After a class key and a first bare identifier A (read; its end already marked): a second bare
/// identifier followed by `{`, on the same or a later line, has no ordinary reading other than `A`
/// annotating that name, once the block it opens is certified a member declaration list. Stacked
/// annotations and a base-clause after the name fall back to the ordinary GLR reading.
static bool class_leading_annotation_follows(TSLexer *lexer) {
    skip_horizontal_space(lexer);
    Word word;
    if (!is_identifier_start(lexer->lookahead) || !read_word(lexer, &word) || word_in(&word, KEYWORDS)) {
        return false;
    }
    bool newline = false;
    return skip_trivia(lexer, &newline) && lexer->lookahead == '{' && class_body_members_follow(lexer);
}

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

////////////////////////////////////////////////////////////////////////////////////////////////////
// Types inside expressions.

/// After `typename` (consumed): a dependent name, possibly qualified and templated, followed by `(`
/// or `{` begins a functional cast such as `typename T::type(x)` or `typename T::type{}`.
static bool dependent_cast_follows(TSLexer *lexer, bool braces_only) {
    Word word;
    skip_horizontal_space(lexer);
    if (lexer->lookahead == ':' && !skip_scope_operator(lexer)) {
        return false;
    }
    bool qualified = false;
    for (;;) {
        skip_horizontal_space(lexer);
        if (!read_word(lexer, &word)) {
            return false;
        }
        if (word_is(&word, "template")) {
            skip_horizontal_space(lexer);
            if (!read_word(lexer, &word)) {
                return false;
            }
        }
        if (!can_name_type(&word)) {
            return false;
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '<' && !skip_template_arguments(lexer)) {
            return false;
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead != ':') {
            break;
        }
        if (!skip_scope_operator(lexer)) {
            return false;
        }
        qualified = true;
    }
    return qualified && (lexer->lookahead == '{' || (!braces_only && lexer->lookahead == '('));
}

/// After a word: the rest of an unnamed pointer to member, `::*` or `::B::*`, then cv-qualifiers and
/// a token that ends the type, as in `X<R C::*>` or `R (C::*)()`. A name after the `*` leaves
/// `S::*p` to the named declarator, and a word followed by anything else stays an identifier.
static bool member_pointer_follows(TSLexer *lexer) {
    Word word;
    for (;;) {
        if (lexer->lookahead != ':' || !skip_scope_operator(lexer)) {
            return false;
        }
        skip_horizontal_space(lexer);
        if (lexer->lookahead == '*') {
            break;
        }
        if (!read_word(lexer, &word) || word_in(&word, KEYWORDS)) {
            return false;
        }
    }
    advance(lexer);
    for (;;) {
        skip_horizontal_space(lexer);
        if (!is_identifier_start(lexer->lookahead)) {
            break;
        }
        if (!read_word(lexer, &word) || !is_cv_qualifier(&word)) {
            return false;
        }
    }
    int32_t next = lexer->lookahead;
    return next == ',' || next == ')' || next == '>' || next == ';';
}

#endif // TREE_SITTER_CPP_SCANNER_DECLARATIONS_H
