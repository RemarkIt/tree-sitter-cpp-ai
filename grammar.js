/**
 * @file C++ grammar for tree-sitter
 * @author Max Brunsfeld <maxbrunsfeld@gmail.com>
 * @author Amaan Qureshi <amaanq12@gmail.com>
 * @author John Drouhard <john@drouhard.dev>
 * @author Pablo Hugen <pabloashugen@protonmail.com>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const C = require('tree-sitter-c/grammar');

const PREC = Object.assign(C.PREC, {
  LAMBDA: 18,
  NEW: C.PREC.CALL + 1,
  STRUCTURED_BINDING: -1,
  THREE_WAY: C.PREC.RELATIONAL + 1,
});

const FOLD_OPERATORS = [
  '+', '-', '*', '/', '%',
  '^', '&', '|',
  '=', '<', '>',
  '<<', '>>',
  '+=', '-=', '*=', '/=', '%=', '^=', '&=', '|=',
  '>>=', '<<=',
  '==', '!=', '<=', '>=',
  '&&', '||',
  ',',
  '.*', '->*',
  'or', 'and', 'bitor', 'xor', 'bitand', 'not_eq',
];

const ASSIGNMENT_OPERATORS = [
  '=',
  '*=',
  '/=',
  '%=',
  '+=',
  '-=',
  '<<=',
  '>>=',
  '&=',
  '^=',
  '|=',
  'and_eq',
  'or_eq',
  'xor_eq',
];

const commaSep = C.commaSep;
const commaSep1 = C.commaSep1;

/**
 * Creates a preprocessor directive token identical to the inherited C grammar's.
 *
 * @param {string} command
 * @returns {AliasRule}
 */
function preprocessor(command) {
  return alias(new RegExp('#[ \t]*' + command), '#' + command);
}

/**
 * The inherited C conditional family with never-produced scanner probes. The `#if`
 * probe is valid exactly where a structured conditional can start, so the scanner only
 * takes conditionals in place where the structured rules cannot parse them.
 *
 * @param {string} suffix
 * @param {($: GrammarSymbols<string>) => RuleOrLiteral} content
 * @param {number} precedence
 * @returns {RuleBuilders<string, string>}
 */
function probedPreprocIf(suffix, content, precedence = 0) {
  /**
   * @param {GrammarSymbols<string>} $
   * @returns {ChoiceRule}
   */
  function alternativeBlock($) {
    return choice(
      suffix ? alias($['preproc_else' + suffix], $.preproc_else) : $.preproc_else,
      suffix ? alias($['preproc_elif' + suffix], $.preproc_elif) : $.preproc_elif,
      suffix ? alias($['preproc_elifdef' + suffix], $.preproc_elifdef) : $.preproc_elifdef,
    );
  }

  return {
    ['preproc_if' + suffix]: $ => prec(precedence, seq(
      optional($._preproc_if_probe),
      preprocessor('if'),
      field('condition', $._preproc_expression),
      choice('\n', $._preproc_end_of_input),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
      preprocessor('endif'),
    )),

    ['preproc_ifdef' + suffix]: $ => prec(precedence, seq(
      optional($._preproc_if_probe),
      choice(preprocessor('ifdef'), preprocessor('ifndef')),
      field('name', macroName($)),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
      preprocessor('endif'),
    )),

    ['preproc_else' + suffix]: $ => prec(precedence, seq(
      preprocessor('else'),
      repeat(content($)),
    )),

    ['preproc_elif' + suffix]: $ => prec(precedence, seq(
      preprocessor('elif'),
      field('condition', $._preproc_expression),
      choice('\n', $._preproc_end_of_input),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
    )),

    ['preproc_elifdef' + suffix]: $ => prec(precedence, seq(
      choice(preprocessor('elifdef'), preprocessor('elifndef')),
      field('name', macroName($)),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
    )),
  };
}

/**
 * A directive on the last line of a file may end at end of input instead of a newline.
 *
 * @param {GrammarSymbols<string>} $
 * @param {SeqRule} original
 * @returns {SeqRule}
 */
function withInputEndLine($, original) {
  const members = original.members;
  return seq(...members.slice(0, -1), choice(members[members.length - 1], $._preproc_end_of_input));
}

/**
 * A macro name, which may spell a reserved word such as `long`.
 *
 * @param {GrammarSymbols<string>} $
 *
 * @returns {Rule}
 */
function macroName($) {
  return reserved('macro_name', $.identifier);
}

/**
 * Constructor/destructor definition tail shared by restricted and block forms.
 *
 * @param {GrammarSymbols<string>} $
 * @param {RuleOrLiteral} declarator
 * @returns {SeqRule}
 */
function constructorDefinition($, declarator) {
  return seq(
    repeat($._constructor_specifiers),
    field('declarator', declarator),
    choice(
      seq(
        optional($.field_initializer_list),
        field('body', $.compound_statement),
      ),
      alias($.constructor_try_statement, $.try_statement),
      $.default_method_clause,
      $.delete_method_clause,
      $.pure_virtual_clause,
    ),
  );
}

module.exports = grammar(C, {
  name: 'cpp',

  externals: $ => [
    $.raw_string_delimiter,
    $.raw_string_content,
    $._leading_annotation,
    $._declarator_annotation,
    // Never produced: its validity tells the scanner that an expression may start here.
    $._expression_start_probe,
    $._macro_invocation_callee,
    // Never produced: its validity marks a braced-initializer element position.
    $._initializer_start_probe,
    $._macro_type_callee,
    // A conditional that cannot be parsed in place: the directive line or `#endif` closing it.
    $.preproc_inline_conditional,
    // A conditional's directive lines and every branch except the last, kept opaque.
    $.preproc_inactive_branch,
    // Never produced: validity marks where a structured `#if` can be parsed.
    $._preproc_if_probe,
    // Zero-width end of a directive on the last line; its validity also marks a directive line
    // that still needs its newline.
    $._preproc_end_of_input,
    // After a class key: `A` in `class A(...) Name`, certified by the name after the arguments.
    $._class_annotation_callee,
    // After a class key: `A` in `class A Name {`, certified by a member declaration's own `;`
    // inside the block, which a braced initializer of `A`'s own declarator never contains.
    $._class_leading_annotation,
    // After a function declarator's parameters: `A` in `f() A {` or `f() A(x);`, certified by a
    // token that can only end or continue a function declarator after the annotation.
    $._function_suffix_annotation,
    // After a call expression: `B` in `A(x) B(y)`, certified by the balanced arguments after it.
    $._juxtaposed_callee,
    // A conditional directive's macro argument holding tokens no expression can, as in `F(?)`.
    $._preproc_raw_argument,
    // At a class member's start: `S` in `S:` ending its line, a macro standing for an access
    // specifier such as Qt's `signals:`. An unnamed bit-field always has its width on the line.
    $._section_label,
    // Before a declaration's type: `A` in `A(...) size_t n`, certified by a keyword type, a
    // specifier, or a type and its declarator after the arguments on the same line.
    $._leading_annotation_callee,
    // The balanced arguments of such a call, kept opaque: annotation languages such as SAL's
    // `_Post_satisfies_(return == 0)` are not C++ expressions.
    $._annotation_arguments,
    // Zero-width, before `typename` in an expression: certified when the dependent name is followed
    // by `(`, as in `typename T::type(x)`, so a functional cast never forks a type reading.
    $._typename_cast,
    // Never produced: its validity marks where a template argument, possibly a type, may start.
    $._template_argument_probe,
    // Returned for words the scanner has already read, so they are not lexed twice.
    $.identifier,
    // Never produced: its validity marks where a word could instead be a literal suffix.
    $.literal_suffix,
  ],

  extras: ($, original) => original.concat([
    $.preproc_inline_conditional,
    $.preproc_inactive_branch,
  ]),

  conflicts: $ => [
    // C
    [$.type_specifier, $._declarator],
    [$.type_specifier, $.expression],
    [$.sized_type_specifier],
    [$.attributed_statement],
    [$._declaration_modifiers, $.attributed_statement],
    [$._declaration_modifiers, $.using_declaration],
    [$._declaration_modifiers, $.attributed_statement, $.using_declaration],
    [$._top_level_item, $._top_level_statement],
    [$._block_item, $.statement],
    [$.type_qualifier, $.extension_expression],

    // C++
    [$.template_function, $.template_type],
    [$.template_function, $.template_type, $.expression],
    [$.template_function, $.template_type, $.qualified_identifier],
    [$.template_function, $.template_type, $.qualified_identifier, $.qualified_type_identifier],
    [$.template_type, $.qualified_type_identifier],
    [$.qualified_type_identifier, $.qualified_identifier],
    [$.comma_expression, $.initializer_list],
    [$.expression, $._declarator],
    [$.expression, $.structured_binding_declarator],
    [$.expression, $._declarator, $.type_specifier],
    [$.expression, $.identifier_parameter_pack_expansion],
    [$.expression, $._lambda_capture_identifier],
    [$.expression, $._lambda_capture],
    [$.expression, $.structured_binding_declarator, $._lambda_capture_identifier],
    [$.structured_binding_declarator, $._lambda_capture_identifier],
    [$.parameter_list, $.argument_list],
    [$.type_specifier, $.call_expression],
    [$._declaration_specifiers, $._constructor_specifiers],
    [$._binary_fold_operator, $._fold_operator],
    [$._function_declarator_seq],
    [$.type_specifier, $.sized_type_specifier],
    [$.initializer_pair, $.comma_expression],
    [$.expression_statement, $._for_statement_body],
    [$.init_statement, $._for_statement_body],
    [$.field_expression, $.template_method, $.template_type],
    [$.field_expression, $.template_method],
    [$.qualified_field_identifier, $.template_method, $.template_type],
    [$.type_specifier, $.template_type, $.template_function, $.expression],
    [$.splice_type_specifier, $.splice_expression],
    // An unexpanded class-head annotation competes with an elaborated class name.
    [$._class_name, $.uncertain_specifier],
    // Keyword-type macro arguments compete with unnamed parameters.
    [$.type_specifier, $._keyword_type_argument],
    [$.type_specifier, $._sized_type_argument],
    [$.type_specifier, $._dependent_type_argument],
    [$.type_qualifier, $._qualified_type_argument],
    // `operator=(x)` calls an operator function where a typeless declaration may also start.
    [$.call_expression, $._constructor_name],
    [$.call_expression, $._declarator],
    // `unsigned(` may begin a sized type or a functional cast.
    [$.sized_type_specifier, $.call_expression],
    // Constructor names compete with called expressions and type names before `(`.
    [$.type_specifier, $.expression, $._constructor_name],
    [$.expression, $._constructor_name],
    [$.type_specifier, $._constructor_name],
    // `T* p(` may declare a function returning a pointer or directly initialize a pointer.
    [$.pointer_declarator, $.function_declarator, $._direct_initialized_pointer_declarator],
    [$.function_declarator, $._direct_initialized_reference_declarator, $.reference_declarator],
    // A macro name after a string literal may continue the concatenation.
    [$._string, $.concatenated_string],
    // `"a" M(x)` may concatenate a macro call or call a function named by `"a" M`.
    [$.concatenated_string, $._string_macro_call],
  ],

  inline: ($, original) => original.concat([
    $._namespace_identifier,
  ]),

  reserved: {
    // Size modifiers never name anything, so `f(unsigned(v))` cannot declare a function `unsigned`.
    global: _ => ['signed', 'unsigned', 'long', 'short'],
    // The preprocessor may define and test macros spelled like keywords.
    macro_name: _ => [],
  },

  precedences: $ => [
    [$.argument_list, $.type_qualifier],
    [$._expression_not_binary, $._class_name],
    // Before `(` an indirection completes only for direct initialization; an ordinary pointer or
    // reference declarator never wraps the declarator of a following function declarator.
    [$._direct_initialized_pointer_declarator, $.pointer_declarator],
    [$._direct_initialized_reference_declarator, $.reference_declarator],
  ],

  rules: {
    _top_level_item: ($, original) => choice(
      ...original.members.filter((member) => member.content?.name != '_old_style_function_definition'),
      $.uncertain_macro_invocation,
      $.namespace_definition,
      $.concept_definition,
      $.namespace_alias_definition,
      $.using_declaration,
      $.alias_declaration,
      $.static_assert_declaration,
      $.consteval_block_declaration,
      $.template_declaration,
      $.template_instantiation,
      $.module_declaration,
      $.export_declaration,
      $.import_declaration,
      $.global_module_fragment_declaration,
      $.private_module_fragment_declaration,
      alias($.constructor_or_destructor_definition, $.function_definition),
      alias($.operator_cast_definition, $.function_definition),
      alias($.operator_cast_declaration, $.declaration),
    ),

    _block_item: ($, original) => choice(
      ...original.members.filter((member) =>
        member.content?.name != '_old_style_function_definition' &&
        !member.name.startsWith('preproc_if'),
      ),
      $.namespace_definition,
      $.concept_definition,
      $.namespace_alias_definition,
      $.using_declaration,
      $.alias_declaration,
      $.static_assert_declaration,
      $.consteval_block_declaration,
      $.template_declaration,
      $.template_instantiation,
      $.export_declaration,
      $.import_declaration,
      alias($.preproc_if_in_block, $.preproc_if),
      alias($.preproc_ifdef_in_block, $.preproc_ifdef),
      alias($._block_constructor_or_destructor_definition, $.function_definition),
      alias($.operator_cast_definition, $.function_definition),
      alias($.operator_cast_declaration, $.declaration),
    ),

    ...probedPreprocIf('', $ => $._top_level_item),
    ...probedPreprocIf('_in_block', $ => $._block_item),
    ...probedPreprocIf('_in_field_declaration_list', $ => $._field_declaration_list_item),
    ...probedPreprocIf('_in_enumerator_list', $ => seq($.enumerator, ',')),
    ...probedPreprocIf('_in_enumerator_list_no_comma', $ => $.enumerator, -1),

    preproc_include: ($, original) => withInputEndLine($, original),

    // C23 and C++23 allow a label at the end of a compound statement, as in `done: }`; a
    // conditional group may likewise end with one.
    labeled_statement: $ => prec.right(seq(
      field('label', $._statement_identifier),
      ':',
      optional(choice($.declaration, $.statement)),
    )),
    // `__has_include(<header>)` and `__has_include("header")` take a header name. Before
    // expansion a macro argument is any balanced token sequence, as in `F(?)` or
    // `__has_cpp_attribute(gnu::cold)`; the scanner keeps one that no expression can hold opaque.
    preproc_argument_list: $ => seq(
      '(',
      commaSep(choice(
        $._preproc_expression,
        $.system_lib_string,
        $.string_literal,
        alias($._preproc_raw_argument, $.preproc_arg),
      )),
      ')',
    ),
    preproc_defined: $ => choice(
      prec(PREC.CALL, seq('defined', '(', macroName($), ')')),
      seq('defined', macroName($)),
    ),
    // A comment, possibly spanning lines, may separate parts of a continued macro body.
    preproc_def: $ => seq(
      preprocessor('define'),
      field('name', macroName($)),
      field('value', repeat($.preproc_arg)),
      choice(token.immediate(/\r?\n/), $._preproc_end_of_input),
    ),
    preproc_function_def: $ => seq(
      preprocessor('define'),
      field('name', macroName($)),
      field('parameters', $.preproc_params),
      field('value', repeat($.preproc_arg)),
      choice(token.immediate(/\r?\n/), $._preproc_end_of_input),
    ),
    preproc_call: ($, original) => withInputEndLine($, original),

    // A macro invocation without a terminating semicolon, with or without arguments. The scanner
    // certifies the name only when no ordinary expression or declaration can continue after it;
    // ordinary arguments, callees and nested blocks stay parsed.
    uncertain_macro_invocation: $ => prec.right(seq(
      field('macro', alias($._macro_invocation_callee, $.identifier)),
      optional(field('arguments', $.argument_list)),
      optional(field('body', $.compound_statement)),
      optional(';'),
    )),

    // A function-like macro used as a type: the scanner certifies it only when a declarator
    // follows on the same line, which an invocation expression can never be followed by.
    uncertain_macro_type: $ => seq(
      field('macro', alias($._macro_type_callee, $.identifier)),
      field('arguments', $.argument_list),
    ),

    initializer_list: $ => seq(
      '{',
      commaSep(choice(
        $.initializer_pair,
        $.expression,
        $.initializer_list,
        $._initializer_start_probe,
      )),
      optional(','),
      '}',
    ),

    // Types

    placeholder_type_specifier: $ => prec(1, seq(
      field('constraint', optional(choice(
        alias($.qualified_type_identifier, $.qualified_identifier),
        $.template_type,
        $._type_identifier,
      ))),
      choice($.auto, alias($.decltype_auto, $.decltype)),
    )),

    auto: _ => 'auto',
    decltype_auto: $ => seq(
      'decltype',
      '(',
      $.auto,
      ')',
    ),
    // `offsetof(T, a.b)` and `offsetof(T, a[1])` name a member path, as Clang and GCC accept.
    offsetof_expression: $ => prec(PREC.OFFSETOF, seq(
      'offsetof',
      '(',
      field('type', $.type_descriptor),
      ',',
      field('member', choice($._field_identifier, $.field_expression, $.subscript_expression)),
      ')',
    )),

    // `decltype(a.size(), std::true_type())` takes a comma expression, as in SFINAE return types.
    decltype: $ => seq(
      'decltype',
      '(',
      choice($.expression, $.comma_expression),
      ')',
    ),

    type_specifier: $ => choice(
      $.uncertain_macro_type,
      $.struct_specifier,
      $.union_specifier,
      $.enum_specifier,
      $.class_specifier,
      $.sized_type_specifier,
      $.primitive_type,
      $.template_type,
      $.dependent_type,
      $.splice_type_specifier,
      $.placeholder_type_specifier,
      $.decltype,
      prec.right(choice(
        alias($.qualified_type_identifier, $.qualified_identifier),
        $._type_identifier,
      )),
    ),

    type_qualifier: (_, original) => choice(
      original,
      'mutable',
      'constinit',
      'consteval',
    ),

    type_descriptor: (_, original) => prec.right(original),

    attribute: ($, original) => seq(
      optional(seq('using', field('namespace', $.identifier), ':')),
      ...original.members,
    ),

    annotation: $ => seq('=', $.expression),

    attribute_declaration: ($, original) => choice(
      original,
      seq('[[', commaSep1($.annotation), ']]'),
    ),

    // When used in a trailing return type, these specifiers can now occur immediately before
    // a compound statement. This introduces a shift/reduce conflict that needs to be resolved
    // with an associativity.
    _class_declaration: $ => seq(
      repeat(choice($.attribute_specifier, $.alignas_qualifier)),
      optional($.ms_declspec_modifier),
      repeat($.attribute_declaration),
      $._class_declaration_item,
    ),
    _class_declaration_item: $ => prec.right(seq(
      choice(
        field('name', $._class_name),
        seq(
          optional(field('name', $._class_name)),
          optional($.virtual_specifier),
          optional($.base_class_clause),
          field('body', $.field_declaration_list),
        ),
        seq(
          repeat1($.uncertain_specifier),
          field('name', $._class_name),
          optional($.virtual_specifier),
          optional($.base_class_clause),
          field('body', $.field_declaration_list),
        ),
        // The common single-annotation, no-base-clause shape (e.g. `struct API_EXPORT Options {`):
        // the scanner certifies it outright, so this never forks a GLR version like the choice above.
        seq(
          alias($._class_leading_uncertain_specifier, $.uncertain_specifier),
          field('name', $._class_name),
          field('body', $.field_declaration_list),
        ),
        // `class M(args) Name` has no reading without an annotation, so unlike `class A b;`
        // it may also declare the class without a body.
        seq(
          repeat($.uncertain_specifier),
          alias($._uncertain_specifier_call, $.uncertain_specifier),
          repeat(choice($.uncertain_specifier, alias($._uncertain_specifier_call, $.uncertain_specifier))),
          field('name', $._class_name),
          optional(seq(
            optional($.virtual_specifier),
            optional($.base_class_clause),
            field('body', $.field_declaration_list),
          )),
        ),
      ),
      optional($.attribute_specifier),
    )),

    class_specifier: $ => seq(
      'class',
      $._class_declaration,
    ),

    union_specifier: $ => seq(
      'union',
      $._class_declaration,
    ),

    struct_specifier: $ => seq(
      'struct',
      $._class_declaration,
    ),

    _class_name: $ => prec.right(choice(
      $._type_identifier,
      $.template_type,
      $.splice_type_specifier,
      alias($.qualified_type_identifier, $.qualified_identifier),
    )),

    // Member templates, friends and namespace-scope templates may be defaulted or deleted, as in
    // `template <class T> void f(T) = delete;`, the way inline methods already are.
    function_definition: ($, original) => ({
      ...original,
      members: original.members.map(
        (e) => e.name !== 'body' ?
          e :
          choice(
            field('body', choice(e.content, $.try_statement)),
            $.default_method_clause,
            $.delete_method_clause,
          )),
    }),

    declaration: $ => seq(
      $._declaration_specifiers,
      commaSep1(field('declarator', choice(
        seq(
          // C uses _declaration_declarator here for some nice macro parsing in function declarators,
          // but this causes a world of pain for C++ so we'll just stick to the normal _declarator here.
          optional($.ms_call_modifier),
          $._declarator,
          optional($.gnu_asm_expression),
        ),
        $.init_declarator,
      ))),
      ';',
    ),

    virtual_specifier: _ => choice(
      'final', // the only legal value here for classes
      'override', // legal for functions in addition to final, plus permutations.
    ),

    _declaration_modifiers: ($, original) => choice(
      original,
      'virtual',
    ),

    // A plain identifier beside the declared type and declarator cannot be ordinary C++
    // syntax: it is an unexpanded annotation. Keep it explicit; never guess its expansion.
    uncertain_specifier: $ => prec.dynamic(-1, $.identifier),

    _uncertain_specifier_call: $ => seq(
      alias($._class_annotation_callee, $.identifier),
      field('arguments', $.argument_list),
    ),

    // The scanner certifies a leading annotation only when what follows cannot be a type and its
    // declarator: a keyword type on the same line, a qualified or templated type with a declarator
    // after it, or a type and declarator after a specifier keyword. So neither ordinary parsing nor
    // error recovery can invent one before a plain type name.
    _leading_uncertain_specifier: $ => alias($._leading_annotation, $.identifier),

    // A call before a declaration's type, as in SAL's `_When_(p != nullptr, ...) size_t n`.
    _leading_uncertain_specifier_call: $ => seq(
      alias($._leading_annotation_callee, $.identifier),
      field('arguments', alias($._annotation_arguments, $.uncertain_arguments)),
    ),

    // Certified by the scanner only when a second bare identifier (the prospective class name) is
    // directly followed by `{`, and that block's own `;` proves it a member declaration list
    // rather than a braced initializer of an elaborated-type-specifier's plain declarator.
    _class_leading_uncertain_specifier: $ => alias($._class_leading_annotation, $.identifier),

    // Leading annotations may interleave with modifiers, so wherever a type can still begin the
    // scanner sees this token as valid and does not certify a declarator annotation instead.
    _declaration_specifiers: $ => prec.right(seq(
      repeat(choice(
        $._declaration_modifiers,
        alias($._leading_uncertain_specifier, $.uncertain_specifier),
        alias($._leading_uncertain_specifier_call, $.uncertain_specifier),
      )),
      field('type', $.type_specifier),
      repeat($._declaration_modifiers),
    )),

    // Certified by the scanner only where neither a type nor an expression can begin, when the
    // identifier is followed by another declarator token rather than ending the declarator.
    _declarator_uncertain_specifier: $ => alias($._declarator_annotation, $.identifier),

    // The negative dynamic precedence keeps ordinary readings, e.g. the product in `a * b * c;`.
    annotated_declarator: $ => prec.dynamic(-3, prec.right(seq(
      field('annotation', alias($._declarator_uncertain_specifier, $.uncertain_specifier)),
      field('declarator', $._declarator),
    ))),
    annotated_field_declarator: $ => prec.dynamic(-3, prec.right(seq(
      field('annotation', alias($._declarator_uncertain_specifier, $.uncertain_specifier)),
      field('declarator', $._field_declarator),
    ))),
    annotated_type_declarator: $ => prec.dynamic(-3, prec.right(seq(
      field('annotation', alias($._declarator_uncertain_specifier, $.uncertain_specifier)),
      field('declarator', $._type_declarator),
    ))),
    abstract_annotated_declarator: $ => prec.dynamic(-3, prec.right(seq(
      field('annotation', alias($._declarator_uncertain_specifier, $.uncertain_specifier)),
      field('declarator', $._abstract_declarator),
    ))),

    explicit_function_specifier: $ => choice(
      'explicit',
      prec(PREC.CALL, seq(
        'explicit',
        '(',
        $.expression,
        ')',
      )),
    ),

    base_class_clause: $ => seq(
      ':',
      commaSep1(seq(
        repeat($.attribute_declaration),
        optional(choice(
          $.access_specifier,
          seq($.access_specifier, optional('virtual')),
          seq('virtual', optional($.access_specifier)),
        )),
        $._class_name,
        optional('...'),
      )),
    ),

    enum_specifier: $ => prec.right(seq(
      'enum',
      optional(choice('class', 'struct')),
      choice(
        seq(
          field('name', $._class_name),
          optional($._enum_base_clause),
          optional(field('body', $.enumerator_list)),
        ),
        // An unnamed enumeration may still fix its base, as in `enum : uint32_t { A = 1 };`.
        seq(optional($._enum_base_clause), field('body', $.enumerator_list)),
      ),
      optional($.attribute_specifier),
    )),

    _enum_base_clause: $ => prec.left(seq(
      ':',
      field('base', choice(
        alias($.qualified_type_identifier, $.qualified_identifier),
        $._type_identifier,
        $.primitive_type,
        $.sized_type_specifier,
      )),
    )),

    // The `auto` storage class is removed in C++0x in order to allow for the `auto` type.
    storage_class_specifier: (_, original) => choice(
      ...original.members.filter((member) => member.value !== 'auto'),
      'thread_local',
    ),

    dependent_type: $ => prec.dynamic(-1, prec.right(seq(
      'typename',
      $.type_specifier,
    ))),

    // Declarations

    module_name: $ => seq(
      $.identifier,
      repeat(seq('.', $.identifier),
      ),
    ),

    module_partition: $ => seq(
      ':',
      $.module_name,
    ),

    module_declaration: $ => seq(
      optional('export'),
      'module',
      field('name', $.module_name),
      field('partition', optional($.module_partition)),
      optional($.attribute_declaration),
      ';',
    ),

    export_declaration: $ => prec(1, seq(
      'export',
      choice($._block_item, $.declaration_list),
    )),

    import_declaration: $ => seq(
      'import',
      choice(
        field('name', $.module_name),
        field('partition', $.module_partition),
        field('header', choice(
          $.string_literal,
          $.system_lib_string,
        )),
      ),
      optional($.attribute_declaration),
      ';',
    ),

    global_module_fragment_declaration: _ => seq('module', ';'),
    private_module_fragment_declaration: _ => seq('module', ':', 'private', ';'),

    template_declaration: $ => seq(
      'template',
      field('parameters', $.template_parameter_list),
      optional($.requires_clause),
      choice(
        $._empty_declaration,
        $.alias_declaration,
        $.declaration,
        $.template_declaration,
        $.function_definition,
        $.concept_definition,
        $.friend_declaration,
        alias($.constructor_or_destructor_declaration, $.declaration),
        alias($.constructor_or_destructor_definition, $.function_definition),
        alias($.operator_cast_declaration, $.declaration),
        alias($.operator_cast_definition, $.function_definition),
      ),
    ),

    // `template class X<int>;` instantiates a class and has no declarator.
    template_instantiation: $ => prec(1, seq(
      optional('extern'),
      'template',
      choice(
        seq(optional($._declaration_specifiers), field('declarator', $._declarator)),
        $._declaration_specifiers,
      ),
      ';',
    )),

    template_parameter_list: $ => seq(
      '<',
      commaSep(choice(
        $.parameter_declaration,
        $.optional_parameter_declaration,
        $.type_parameter_declaration,
        $.variadic_parameter_declaration,
        $.variadic_type_parameter_declaration,
        $.optional_type_parameter_declaration,
        $.template_template_parameter_declaration,
      )),
      alias(token(prec(1, '>')), '>'),
    ),

    type_parameter_declaration: $ => prec(1, seq(
      choice('typename', 'class'),
      optional($._type_identifier),
    )),

    variadic_type_parameter_declaration: $ => prec(1, seq(
      choice('typename', 'class'),
      '...',
      optional($._type_identifier),
    )),

    optional_type_parameter_declaration: $ => seq(
      choice('typename', 'class'),
      optional(field('name', $._type_identifier)),
      '=',
      field('default_type', $.type_specifier),
      optional(field('declarator', choice($.abstract_pointer_declarator, $.abstract_reference_declarator))),
    ),

    template_template_parameter_declaration: $ => seq(
      'template',
      field('parameters', $.template_parameter_list),
      choice(
        $.type_parameter_declaration,
        $.variadic_type_parameter_declaration,
        $.optional_type_parameter_declaration,
      ),
    ),

    parameter_list: $ => seq(
      '(',
      commaSep(choice(
        $.parameter_declaration,
        $.explicit_object_parameter_declaration,
        $.optional_parameter_declaration,
        $.variadic_parameter_declaration,
        '...',
      )),
      ')',
    ),

    explicit_object_parameter_declaration: $ => seq(
      $.this,
      $.parameter_declaration,
    ),

    optional_parameter_declaration: $ => seq(
      $._declaration_specifiers,
      // An unnamed pointer parameter may take a default, as in `enable_if_t<B>* = nullptr`.
      field('declarator', optional(choice(
        $._declarator, $.abstract_reference_declarator, $.abstract_pointer_declarator,
      ))),
      '=',
      field('default_value', choice($.expression, $.initializer_list)),
    ),

    variadic_parameter_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', choice(
        $.variadic_declarator,
        alias($.variadic_reference_declarator, $.reference_declarator),
      )),
    ),

    variadic_declarator: $ => seq(
      '...',
      optional($.identifier),
    ),

    variadic_reference_declarator: $ => seq(
      choice('&&', '&'),
      $.variadic_declarator,
    ),

    init_declarator: ($, original) => choice(
      original,
      seq(
        field('declarator', $._declarator),
        field('value', choice(
          $.argument_list,
          $.initializer_list,
        )),
      ),
      seq(
        field('declarator', $._direct_initialized_declarator),
        field('value', $.argument_list),
      ),
    ),

    // `T* p(expr)` and `T& r(expr)`: the indirection completes before `(`, so the parentheses
    // can hold an expression. Ordinary pointer and reference declarators instead extend over
    // `(` to declare functions returning them, and that reading wins whenever it parses. With
    // no dynamic precedence of its own, this reading also yields to the expression `c * d(5)`
    // wherever the type could be an operand, keeping the inherited C reading.
    _direct_initialized_declarator: $ => choice(
      alias($._direct_initialized_pointer_declarator, $.pointer_declarator),
      alias($._direct_initialized_reference_declarator, $.reference_declarator),
    ),

    _direct_initialized_pointer_declarator: $ => seq(
      optional($.ms_based_modifier),
      '*',
      repeat($.ms_pointer_modifier),
      repeat($.type_qualifier),
      field('declarator', choice($._declarator, $._direct_initialized_declarator)),
    ),

    _direct_initialized_reference_declarator: $ => seq(
      choice('&', '&&'),
      choice($._declarator, $._direct_initialized_declarator),
    ),

    operator_cast: $ => prec.right(1, seq(
      'operator',
      $._declaration_specifiers,
      field('declarator', $._abstract_declarator),
    )),

    // Avoid ambiguity between compound statement and initializer list in a construct like:
    //   A b {};
    compound_statement: (_, original) => prec(-1, original),

    field_initializer_list: $ => seq(
      ':',
      commaSep1($.field_initializer),
    ),

    field_initializer: $ => prec(1, seq(
      choice(
        $._field_identifier,
        $.template_method,
        alias($.qualified_field_identifier, $.qualified_identifier),
      ),
      choice($.initializer_list, $.argument_list),
      optional('...'),
    )),

    _field_declaration_list_item: ($, original) => choice(
      original,
      $.uncertain_macro_invocation,
      $.template_declaration,
      alias($.inline_method_definition, $.function_definition),
      alias($.constructor_or_destructor_definition, $.function_definition),
      alias($.constructor_or_destructor_declaration, $.declaration),
      alias($.operator_cast_definition, $.function_definition),
      alias($.operator_cast_declaration, $.declaration),
      $.friend_declaration,
      seq($.access_specifier, optional(alias($._access_annotation, $.uncertain_specifier)), ':'),
      seq(alias($._section_annotation, $.uncertain_specifier), ':'),
      $.alias_declaration,
      $.using_declaration,
      $.type_definition,
      $.static_assert_declaration,
      $.consteval_block_declaration,
      ';',
    ),

    // Members take a calling convention before the declarator, as declarations do.
    field_declaration: $ => seq(
      $._declaration_specifiers,
      commaSep(seq(
        optional($.ms_call_modifier),
        field('declarator', $._field_declarator),
        optional(choice(
          $.bitfield_clause,
          field('default_value', $.initializer_list),
          seq('=', field('default_value', choice($.expression, $.initializer_list))),
        )),
      )),
      optional($.attribute_specifier),
      ';',
    ),

    inline_method_definition: $ => seq(
      $._declaration_specifiers,
      optional($.ms_call_modifier),
      field('declarator', $._field_declarator),
      choice(
        field('body', choice($.compound_statement, $.try_statement)),
        $.default_method_clause,
        $.delete_method_clause,
        $.pure_virtual_clause,
      ),
    ),

    // A certified leading annotation may also precede a constructor or conversion function, as in
    // `WI_NODISCARD explicit operator bool() const`.
    _constructor_specifiers: $ => choice(
      $._declaration_modifiers,
      $.explicit_function_specifier,
      alias($._leading_uncertain_specifier, $.uncertain_specifier),
    ),

    operator_cast_definition: $ => seq(
      repeat($._constructor_specifiers),
      field('declarator', choice(
        $.operator_cast,
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
      )),
      field('body', choice($.compound_statement, $.try_statement)),
    ),

    operator_cast_declaration: $ => prec(1, seq(
      repeat($._constructor_specifiers),
      field('declarator', choice(
        $.operator_cast,
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
      )),
      optional(seq('=', field('default_value', $.expression))),
      ';',
    )),

    constructor_try_statement: $ => seq(
      'try',
      optional($.field_initializer_list),
      field('body', $.compound_statement),
      repeat1($.catch_clause),
    ),

    // A typeless declaration names a constructor or destructor directly; nested
    // function or parenthesized pointer declarators need a declared type.
    _constructor_declarator: $ => prec.dynamic(1, seq(
      field('declarator', $._constructor_name),
      $._function_declarator_seq,
    )),

    _constructor_name: $ => choice(
      $.identifier,
      $.qualified_identifier,
      $.destructor_name,
      $.operator_name,
    ),

    constructor_or_destructor_definition: $ => constructorDefinition(
      $, alias($._constructor_declarator, $.function_declarator),
    ),

    // Function bodies keep upstream's block-item recovery shape and GLR order.
    _block_constructor_or_destructor_definition: $ => constructorDefinition($, $.function_declarator),

    constructor_or_destructor_declaration: $ => seq(
      repeat($._constructor_specifiers),
      field('declarator', alias($._constructor_declarator, $.function_declarator)),
      ';',
    ),

    default_method_clause: _ => seq('=', 'default', ';'),
    delete_method_clause: _ => seq('=', 'delete', ';'),
    pure_virtual_clause: _ => seq('=', /0/, ';'),

    friend_declaration: $ => seq(
      optional('constexpr'),
      'friend',
      choice(
        $.declaration,
        $.function_definition,
        seq(
          optional(choice(
            'class',
            'struct',
            'union',
          )),
          $._class_name, ';',
        ),
      ),
    ),

    // MSVC writes the calling convention of a function returning a pointer after the pointer
    // operator, as in `void* __stdcall f()`.
    pointer_declarator: $ => prec.dynamic(1, prec.right(seq(
      optional($.ms_based_modifier),
      '*',
      repeat($.ms_pointer_modifier),
      repeat($.type_qualifier),
      optional($.ms_call_modifier),
      field('declarator', $._declarator),
    ))),

    pointer_field_declarator: $ => prec.dynamic(1, prec.right(seq(
      optional($.ms_based_modifier),
      '*',
      repeat($.ms_pointer_modifier),
      repeat($.type_qualifier),
      optional($.ms_call_modifier),
      field('declarator', $._field_declarator),
    ))),

    // A word between an access specifier and its `:`, as in Qt's `public slots:`, can only be
    // a macro; nothing else may appear there.
    _access_annotation: $ => $.identifier,

    _section_annotation: $ => alias($._section_label, $.identifier),

    // `__declspec(uuid("..."))` and `__declspec(align(16))` take arguments.
    ms_declspec_modifier: $ => seq(
      '__declspec',
      '(',
      $.identifier,
      optional($.argument_list),
      ')',
    ),

    access_specifier: _ => choice(
      'public',
      'private',
      'protected',
    ),

    _declarator: ($, original) => choice(
      original,
      $.annotated_declarator,
      $.reference_declarator,
      $.qualified_identifier,
      $.template_function,
      $.operator_name,
      $.destructor_name,
      $.structured_binding_declarator,
    ),

    _field_declarator: ($, original) => choice(
      original,
      alias($._member_pointer_field_declarator, $.pointer_declarator),
      alias($.annotated_field_declarator, $.annotated_declarator),
      alias($.reference_field_declarator, $.reference_declarator),
      $.template_method,
      $.operator_name,
    ),

    _type_declarator: ($, original) => choice(
      original,
      alias($.annotated_type_declarator, $.annotated_declarator),
      alias($.reference_type_declarator, $.reference_declarator),
    ),

    _abstract_declarator: ($, original) => choice(
      original,
      alias($.abstract_annotated_declarator, $.annotated_declarator),
      $.abstract_reference_declarator,
    ),

    // Pointers to members among members: `int C::*p;` and `void (C::*f)(int);`. Namespace-scope
    // declarators keep upstream's reading; the abstract `T C::*` stays unsupported, since it
    // conflicts with sized types in template parameter lists.
    _member_pointer_field_declarator: $ => prec.dynamic(1, prec.right(seq(
      $._scope_resolution,
      '*',
      repeat($.type_qualifier),
      field('declarator', $._field_declarator),
    ))),

    reference_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._declarator))),
    reference_field_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._field_declarator))),
    reference_type_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._type_declarator))),
    abstract_reference_declarator: $ => prec.right(seq(choice('&', '&&'), optional($._abstract_declarator))),

    structured_binding_declarator: $ => prec.dynamic(PREC.STRUCTURED_BINDING, seq(
      '[', commaSep1($.identifier), ']',
    )),

    ref_qualifier: _ => choice('&', '&&'),

    _function_declarator_seq: $ => seq(
      field('parameters', $.parameter_list),
      optional($._function_attributes_start),
      optional($.ref_qualifier),
      optional($._function_exception_specification),
      optional($._function_attributes_end),
      optional($.trailing_return_type),
      optional($._function_postfix),
    ),

    _function_attributes_start: $ => prec(1, choice(
      seq(repeat1($._function_attribute), repeat($.type_qualifier)),
      seq(repeat($._function_attribute), repeat1($.type_qualifier)),
    )),

    _function_attribute: $ => choice(
      $.attribute_specifier,
      alias($._function_suffix_specifier, $.uncertain_specifier),
    ),

    // An identifier after a function declarator's parameters is never ordinary C++ syntax: it is
    // an unexpanded annotation such as `f() NOEXCEPT_MACRO {` or `f() LOCKS_EXCLUDED(mu);`.
    _function_suffix_specifier: $ => prec.right(seq(
      alias($._function_suffix_annotation, $.identifier),
      optional(field('arguments', $.argument_list)),
    )),

    _function_exception_specification: $ => choice(
      $.noexcept,
      $.throw_specifier,
    ),

    _function_attributes_end: $ => prec.right(seq(
      optional($.gnu_asm_expression),
      choice(
        seq(repeat1($._function_attribute), repeat($.attribute_declaration)),
        seq(repeat($._function_attribute), repeat1($.attribute_declaration)),
      ),
    )),

    _function_postfix: $ => prec.right(choice(
      repeat1($.virtual_specifier),
      $.requires_clause,
    )),

    function_declarator: $ => prec.dynamic(1, seq(
      field('declarator', $._declarator),
      $._function_declarator_seq,
    )),

    function_field_declarator: $ => prec.dynamic(1, seq(
      field('declarator', $._field_declarator),
      $._function_declarator_seq,
    )),

    abstract_function_declarator: $ => choice(
      seq(
        field('declarator', optional($._abstract_declarator)),
        $._function_declarator_seq,
      ),
      // MSVC function type-id with a calling convention, e.g. `BOOL __stdcall(HWND)`.
      // Inside parentheses the inherited `(__stdcall abstract-declarator)` reading wins.
      prec(-1, seq($.ms_call_modifier, $._function_declarator_seq)),
    ),

    trailing_return_type: $ => seq('->', $.type_descriptor),

    noexcept: $ => prec.right(seq(
      'noexcept',
      optional(
        seq(
          '(',
          optional($.expression),
          ')',
        ),
      ),
    )),

    throw_specifier: $ => seq(
      'throw',
      seq(
        '(',
        commaSep($.type_descriptor),
        ')',
      ),
    ),

    template_type: $ => seq(
      field('name', $._type_identifier),
      field('arguments', $.template_argument_list),
    ),

    template_method: $ => seq(
      field('name', choice($._field_identifier, $.operator_name)),
      field('arguments', $.template_argument_list),
    ),

    template_function: $ => seq(
      field('name', $.identifier),
      field('arguments', $.template_argument_list),
    ),

    template_argument_list: $ => seq(
      '<',
      commaSep(choice(
        prec.dynamic(3, $.type_descriptor),
        prec.dynamic(2, alias($.type_parameter_pack_expansion, $.parameter_pack_expansion)),
        prec.dynamic(1, $.expression),
        $._template_argument_probe,
      )),
      alias(token(prec(1, '>')), '>'),
    ),

    namespace_definition: $ => seq(
      optional('inline'),
      'namespace',
      optional($.attribute_declaration),
      field('name', optional(
        choice(
          $._namespace_identifier,
          $.nested_namespace_specifier,
        ))),
      field('body', $.declaration_list),
    ),

    namespace_alias_definition: $ => seq(
      'namespace',
      field('name', $._namespace_identifier),
      '=',
      choice(
        $._namespace_identifier,
        $.nested_namespace_specifier,
        $.splice_specifier,
      ),
      ';',
    ),

    _namespace_specifier: $ => seq(
      optional('inline'),
      $._namespace_identifier,
    ),

    nested_namespace_specifier: $ => prec(1, seq(
      optional($._namespace_specifier),
      '::',
      choice(
        $.nested_namespace_specifier,
        $._namespace_specifier,
      ),
    )),

    using_declaration: $ => seq(
      repeat($.attribute_declaration),
      'using',
      optional(choice('namespace', 'enum')),
      commaSep1(seq(
        choice(
          $.identifier,
          $.qualified_identifier,
          $.splice_type_specifier,
        ),
        optional('...'),
      )),
      ';',
    ),

    alias_declaration: $ => seq(
      'using',
      field('name', $._type_identifier),
      repeat($.attribute_declaration),
      '=',
      field('type', $.type_descriptor),
      ';',
    ),

    static_assert_declaration: $ => seq(
      'static_assert',
      '(',
      field('condition', $.expression),
      optional(seq(
        ',',
        field('message', $._string),
      )),
      ')',
      ';',
    ),

    consteval_block_declaration: $ => seq(
      'consteval',
      field('body', $.compound_statement),
    ),

    concept_definition: $ => seq(
      'concept',
      field('name', $.identifier),
      '=',
      $.expression,
      ';',
    ),

    // Statements

    _top_level_statement: ($, original) => choice(
      original,
      $.co_return_statement,
      $.co_yield_statement,
      $.for_range_loop,
      $.expansion_statement,
      $.try_statement,
      $.throw_statement,
    ),

    _non_case_statement: ($, original) => choice(
      original,
      $.uncertain_macro_invocation,
      $.co_return_statement,
      $.co_yield_statement,
      $.for_range_loop,
      $.expansion_statement,
      $.try_statement,
      $.throw_statement,
    ),

    switch_statement: $ => seq(
      'switch',
      field('condition', $.condition_clause),
      field('body', $.compound_statement),
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $.condition_clause),
      field('body', $._substatement),
    ),

    if_statement: $ => prec.right(seq(
      'if',
      optional('constexpr'),
      field('condition', $.condition_clause),
      field('consequence', $._substatement),
      optional(field('alternative', $.else_clause)),
    )),

    else_clause: $ => seq('else', $._substatement),

    for_statement: ($, original) => ({
      ...original,
      members: original.members.map((e) => e.name !== 'body' ? e : field('body', $._substatement)),
    }),

    do_statement: ($, original) => ({
      ...original,
      members: original.members.map((e) => e.name !== 'body' ? e : field('body', $._substatement)),
    }),

    // A C++ substatement may be a declaration statement, as in `if (ok) emit changed(name);`.
    // Ambiguous statements read as they do in a block: `a * b;` declares, `f(a);` calls.
    _substatement: $ => choice($.statement, $.declaration),

    // Using prec(1) instead of prec.dynamic(1) causes issues with the
    // range loop's declaration specifiers if `int` is passed in, it'll
    // always prefer the standard for loop and give us a parse error.
    _for_statement_body: ($, original) => prec.dynamic(1, original),
    for_range_loop: $ => seq(
      'for',
      '(',
      $._for_range_loop_body,
      ')',
      field('body', $._substatement),
    ),
    _for_range_loop_body: $ => seq(
      field('initializer', optional($.init_statement)),
      $._declaration_specifiers,
      field('declarator', $._declarator),
      ':',
      field('right', choice(
        $.expression,
        $.initializer_list,
      )),
    ),

    init_statement: $ => choice(
      $.alias_declaration,
      $.type_definition,
      $.declaration,
      $.expression_statement,
    ),

    condition_clause: $ => seq(
      '(',
      field('initializer', optional($.init_statement)),
      field('value', choice(
        $.expression,
        $.comma_expression,
        alias($.condition_declaration, $.declaration),
      )),
      ')',
    ),

    condition_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', $._declarator),
      choice(
        seq(
          '=',
          field('value', $.expression),
        ),
        field('value', $.initializer_list),
      ),
    ),

    return_statement: ($, original) => seq(
      choice(
        original,
        seq('return', $.initializer_list, ';'),
      ),
    ),

    co_return_statement: $ => seq(
      'co_return',
      optional($.expression),
      ';',
    ),

    co_yield_statement: $ => seq(
      'co_yield',
      $.expression,
      ';',
    ),

    throw_statement: $ => seq(
      'throw',
      optional($.expression),
      ';',
    ),

    // A handler may be a macro, as in wil's `try { ... } CATCH_LOG();`: only a handler can follow
    // a try block, so a call there is one.
    try_statement: $ => seq(
      'try',
      field('body', $.compound_statement),
      choice(repeat1($.catch_clause), alias($._macro_handler, $.uncertain_macro_invocation)),
    ),

    _macro_handler: $ => prec.right(seq(
      field('macro', $.identifier),
      field('arguments', $.argument_list),
      optional(';'),
    )),

    catch_clause: $ => seq(
      'catch',
      field('parameters', $.parameter_list),
      field('body', $.compound_statement),
    ),

    // Expressions

    _expression_not_binary: ($, original) => choice(
      original,
      $.uncertain_concatenation,
      $._expression_start_probe,
      $.co_await_expression,
      $.requires_expression,
      $.requires_clause,
      $.template_function,
      $.qualified_identifier,
      $.new_expression,
      $.delete_expression,
      $.lambda_expression,
      $.parameter_pack_expansion,
      $.this,
      $.user_defined_literal,
      $.fold_expression,
      $.reflect_expression,
      $.splice_expression,
    ),

    _string: $ => choice(
      $.string_literal,
      $.raw_string_literal,
      $.concatenated_string,
    ),

    raw_string_literal: $ => seq(
      choice('R"', 'LR"', 'uR"', 'UR"', 'u8R"'),
      choice(
        seq(
          field('delimiter', $.raw_string_delimiter),
          '(',
          $.raw_string_content,
          ')',
          $.raw_string_delimiter,
        ),
        seq('(', $.raw_string_content, ')'),
      ),
      '"',
    ),

    subscript_expression: $ => prec(PREC.SUBSCRIPT, seq(
      field('argument', $.expression),
      field('indices', $.subscript_argument_list),
    )),

    subscript_argument_list: $ => seq(
      '[',
      commaSep(choice($.expression, $.initializer_list)),
      ']',
    ),

    call_expression: ($, original) => choice(
      prec.dynamic(1, choice(original, seq(
        field('function', choice($.primitive_type, seq(
          optional('typename'),
          $.splice_type_specifier,
        ))),
        field('arguments', $.argument_list),
      ))),
      // A functional cast such as `unsigned(v)` takes a single-word type. Where a declaration
      // such as `unsigned(v);` or `long(*p) = 0;` is possible, the declaration wins.
      prec.dynamic(PREC.PAREN_DECLARATOR * 2, seq(
        field('function', alias(choice('signed', 'unsigned', 'long', 'short'), $.primitive_type)),
        field('arguments', $.argument_list),
      )),
      // `operator=(x)` and `operator""_s(x)` call an operator function by name. A declaration
      // naming the operator still wins.
      prec.dynamic(PREC.PAREN_DECLARATOR * 2, seq(
        field('function', $.operator_name),
        field('arguments', $.argument_list),
      )),
      // `typename T::type(x)` is a functional cast to a dependent type. The scanner certifies it
      // at `typename` only where neither a declaration nor a template argument, which may be the
      // function type `typename T::type(int)`, can start; a fork there changed template-argument
      // readings in unrelated types.
      seq(
        $._typename_cast,
        field('function', $.dependent_type),
        field('arguments', $.argument_list),
      ),
    ),

    co_await_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', 'co_await'),
      field('argument', $.expression),
    )),

    new_expression: $ => prec.right(PREC.NEW, seq(
      optional('::'),
      'new',
      field('placement', optional($.argument_list)),
      field('type', $.type_specifier),
      field('declarator', optional(choice($.new_declarator, $._new_pointer_declarator))),
      field('arguments', optional(choice(
        $.argument_list,
        $.initializer_list,
      ))),
    )),

    // `new T*[n]`: a new-type-id takes pointer operators greedily, before any array bounds.
    _new_pointer_declarator: $ => prec.right(seq(repeat1('*'), optional($.new_declarator))),

    new_declarator: $ => prec.right(seq(
      '[',
      field('length', $.expression),
      ']',
      optional($.new_declarator),
    )),

    delete_expression: $ => seq(
      optional('::'),
      'delete',
      optional(seq('[', ']')),
      $.expression,
    ),

    field_expression: $ => seq(
      prec(PREC.FIELD, seq(
        field('argument', $.expression),
        field('operator', choice('.', '.*', '->')),
      )),
      field('field', choice(
        prec.dynamic(1, $._field_identifier),
        alias($.qualified_field_identifier, $.qualified_identifier),
        $.destructor_name,
        $.template_method,
        alias($.dependent_field_identifier, $.dependent_name),
        $.operator_name,
        alias($._operator_cast_field, $.operator_cast),
        $.splice_expression,
      )),
    ),

    // `x.operator T()` names a conversion function; the call's arguments follow it.
    _operator_cast_field: $ => prec.right(seq('operator', $._declaration_specifiers)),

    type_requirement: $ => seq('typename', $._class_name),

    compound_requirement: $ => seq(
      '{', $.expression, '}',
      optional('noexcept'),
      optional($.trailing_return_type),
      ';',
    ),

    _requirement: $ => choice(
      alias($.expression_statement, $.simple_requirement),
      $.type_requirement,
      $.compound_requirement,
    ),

    requirement_seq: $ => seq('{', repeat($._requirement), '}'),

    constraint_conjunction: $ => prec.left(PREC.LOGICAL_AND, seq(
      field('left', $._requirement_clause_constraint),
      field('operator', choice('&&', 'and')),
      field('right', $._requirement_clause_constraint)),
    ),

    constraint_disjunction: $ => prec.left(PREC.LOGICAL_OR, seq(
      field('left', $._requirement_clause_constraint),
      field('operator', choice('||', 'or')),
      field('right', $._requirement_clause_constraint)),
    ),

    _requirement_clause_constraint: $ => choice(
      // Primary expressions"
      $.true,
      $.false,
      $._class_name,
      $.fold_expression,
      $.lambda_expression,
      $.requires_expression,

      // Parenthesized expressions
      seq('(', $.expression, ')'),

      // conjunction or disjunction of the above
      $.constraint_conjunction,
      $.constraint_disjunction,
    ),

    requires_clause: $ => seq(
      'requires',
      field('constraint', $._requirement_clause_constraint),
    ),

    requires_parameter_list: $ => seq(
      '(',
      commaSep(choice(
        $.parameter_declaration,
        $.optional_parameter_declaration,
        $.variadic_parameter_declaration,
      )),
      ')',
    ),

    requires_expression: $ => seq(
      'requires',
      field('parameters', optional(alias($.requires_parameter_list, $.parameter_list))),
      field('requirements', $.requirement_seq),
    ),

    lambda_specifier: $ => choice(
      'static',
      'constexpr',
      'consteval',
      'mutable',
    ),

    lambda_declarator: $ => choice(
      // main declarator form, includes parameter list
      seq(
        repeat($.attribute_declaration),
        field('parameters', $.parameter_list),
        // As after a function's parameters, an identifier here is an unexpanded annotation, as in
        // `[&](GUID id) WI_NOEXCEPT {`: lambda specifiers are all keywords.
        repeat(choice($.lambda_specifier, alias($._function_suffix_specifier, $.uncertain_specifier))),
        optional($._function_exception_specification),
        repeat($.attribute_declaration),
        optional($.trailing_return_type),
        optional($.requires_clause),
      ),

      // forms supporting omitted parameter list
      repeat1($.attribute_declaration),
      seq(
        repeat($.attribute_declaration),
        $.trailing_return_type,
      ),
      seq(
        repeat($.attribute_declaration),
        $._function_exception_specification,
        repeat($.attribute_declaration),
        optional($.trailing_return_type),
      ),
      seq(
        repeat($.attribute_declaration),
        repeat1($.lambda_specifier),
        optional($._function_exception_specification),
        repeat($.attribute_declaration),
        optional($.trailing_return_type),
      ),
    ),

    lambda_expression: $ => seq(
      field('captures', $.lambda_capture_specifier),
      optional(seq(
        field('template_parameters', $.template_parameter_list),
        optional(field('constraint', $.requires_clause)),
      )),
      optional(field('declarator', $.lambda_declarator)),
      field('body', $.compound_statement),
    ),

    lambda_capture_specifier: $ => prec(PREC.LAMBDA, seq(
      '[',
      choice(
        $.lambda_default_capture,
        commaSep($._lambda_capture),
        seq(
          $.lambda_default_capture,
          ',', commaSep1($._lambda_capture),
        ),
      ),
      ']',
    )),

    lambda_default_capture: _ => choice('=', '&'),

    _lambda_capture_identifier: $ => seq(
      optional('&'),
      choice(
        $.identifier,
        $.qualified_identifier,
        alias($.identifier_parameter_pack_expansion, $.parameter_pack_expansion),
      ),
    ),

    lambda_capture_initializer: $ => seq(
      optional('&'),
      optional('...'),
      field('left', $.identifier),
      '=',
      field('right', $.expression),
    ),

    _lambda_capture: $ => choice(
      seq(optional('*'), $.this),
      $._lambda_capture_identifier,
      $.lambda_capture_initializer,
    ),

    _fold_operator: _ => choice(...FOLD_OPERATORS),
    _binary_fold_operator: _ => choice(...FOLD_OPERATORS.map((operator) => seq(field('operator', operator), '...', operator))),

    _unary_left_fold: $ => seq(
      field('left', '...'),
      field('operator', $._fold_operator),
      field('right', $.expression),
    ),
    _unary_right_fold: $ => seq(
      field('left', $.expression),
      field('operator', $._fold_operator),
      field('right', '...'),
    ),
    _binary_fold: $ => seq(
      field('left', $.expression),
      $._binary_fold_operator,
      field('right', $.expression),
    ),

    fold_expression: $ => seq(
      '(',
      choice(
        $._unary_right_fold,
        $._unary_left_fold,
        $._binary_fold,
      ),
      ')',
    ),

    parameter_pack_expansion: $ => prec(-1, seq(
      field('pattern', $.expression),
      '...',
    )),

    type_parameter_pack_expansion: $ => seq(
      field('pattern', $.type_descriptor),
      '...',
    ),

    identifier_parameter_pack_expansion: $ => seq(
      field('pattern', $.identifier),
      '...',
    ),

    sizeof_expression: ($, original) => prec.right(PREC.SIZEOF, choice(
      original,
      seq(
        'sizeof', '...',
        '(',
        field('value', $.identifier),
        ')',
      ),
    )),

    unary_expression: ($, original) => choice(
      original,
      prec.left(PREC.UNARY, seq(
        field('operator', choice('not', 'compl')),
        field('argument', $.expression),
      )),
    ),

    binary_expression: ($, original) => {
      const table = [
        ['<=>', PREC.THREE_WAY],
        ['or', PREC.LOGICAL_OR],
        ['and', PREC.LOGICAL_AND],
        ['bitor', PREC.INCLUSIVE_OR],
        ['xor', PREC.EXCLUSIVE_OR],
        ['bitand', PREC.BITWISE_AND],
        ['not_eq', PREC.EQUAL],
        // `ptr->*member` binds looser than casts; sharing multiplication's level only groups the
        // rare `a * b->*c` from the left.
        ['->*', PREC.MULTIPLY],
      ];

      return choice(
        original,
        ...table.map(([operator, precedence]) => {
          return prec.left(precedence, seq(
            field('left', $.expression),
            // @ts-ignore
            field('operator', operator),
            field('right', $.expression),
          ));
        }));
    },

    // C++ alternative operator spellings are ordinary tokens in conditional directives.
    preproc_unary_expression: ($, original) => choice(
      original,
      prec.left(PREC.UNARY, seq(
        field('operator', choice('not', 'compl')),
        field('argument', $._preproc_expression),
      )),
    ),

    preproc_binary_expression: ($, original) => choice(
      original,
      ...[
        ['or', PREC.LOGICAL_OR],
        ['and', PREC.LOGICAL_AND],
        ['bitor', PREC.INCLUSIVE_OR],
        ['xor', PREC.EXCLUSIVE_OR],
        ['bitand', PREC.BITWISE_AND],
        ['not_eq', PREC.EQUAL],
      ].map(([operator, precedence]) => prec.left(precedence, seq(
        field('left', $._preproc_expression),
        // @ts-ignore
        field('operator', operator),
        field('right', $._preproc_expression),
      ))),
    ),

    // The compound_statement is added to parse macros taking statements as arguments, e.g. MYFORLOOP(1, 10, i, { foo(i); bar(i); })
    // Macro arguments may also be empty, as in `M(a, )`.
    argument_list: $ => {
      const argument = choice(
        $.expression,
        $.initializer_list,
        $.compound_statement,
        alias($._keyword_type_argument, $.type_descriptor),
        alias($._sized_type_argument, $.type_descriptor),
        alias($._qualified_type_argument, $.type_descriptor),
        alias($._dependent_type_argument, $.type_descriptor),
        $.uncertain_type_argument,
      );
      return seq(
        '(',
        optional(choice(
          seq(argument, repeat(seq(',', optional(argument)))),
          seq(repeat1(','), optional(seq(argument, repeat(seq(',', optional(argument)))))),
        )),
        ')',
      );
    },

    // GNU attribute arguments are names and expressions, never type-ids, so a keyword such as
    // `const` in `__attribute__((const))` keeps its identifier reading.
    attribute_specifier: $ => seq(
      choice('__attribute__', '__attribute'),
      '(',
      alias($._attribute_argument_list, $.argument_list),
      ')',
    ),

    _attribute_argument_list: $ => seq(
      '(',
      commaSep(choice($.expression, $.initializer_list)),
      ')',
    ),

    // Macro arguments such as `CHECK(bool)` that can only be types. They start with a primitive
    // type so `unsigned(v)` and `__attribute__((const))` keep their identifier readings; any
    // ordinary reading, even a parenthesized declarator, wins over this one.
    _keyword_type_argument: $ => prec.dynamic(PREC.PAREN_DECLARATOR * 2, prec.right(seq(
      field('type', $.primitive_type),
      repeat($.type_qualifier),
      field('declarator', optional(choice($.abstract_pointer_declarator, $.abstract_reference_declarator))),
    ))),

    // Macro arguments such as `va_arg(ap, const char *)`: a leading cv-qualifier begins a type.
    // Like other type arguments, any ordinary reading wins over this one.
    _qualified_type_argument: $ => prec.dynamic(PREC.PAREN_DECLARATOR * 2, prec.right(seq(
      repeat1(alias(choice('const', 'volatile'), $.type_qualifier)),
      field('type', $.type_specifier),
      repeat($.type_qualifier),
      field('declarator', optional(choice($.abstract_pointer_declarator, $.abstract_reference_declarator))),
    ))),

    // Macro arguments such as `CHECK_THROWS_AS(f(), typename Json::out_of_range&)`: a `typename`
    // that the scanner does not certify as a functional cast begins a type. Like other type
    // arguments, any ordinary reading wins over this one.
    _dependent_type_argument: $ => prec.dynamic(PREC.PAREN_DECLARATOR * 2, seq(
      field('type', $.dependent_type),
      field('declarator', optional(choice($.abstract_pointer_declarator, $.abstract_reference_declarator))),
    )),

    // Macro arguments such as `CHECK_THROWS_AS(f(), json::parse_error&)`: no expression ends
    // with `*`, `&` or `&&`, so the argument is a pointer or reference type. Its name keeps the
    // expression shape it was read with, since the decision falls after the name: a type path
    // forked at the name would compete with template readings for parse versions. A second `*`
    // stays a dereference, and a trailing cv-qualifier would compete with `this const` of an
    // explicit object parameter. Like other type arguments, any ordinary reading wins.
    uncertain_type_argument: $ => prec.dynamic(PREC.PAREN_DECLARATOR * 2, seq(
      field('type', $.expression),
      repeat1(choice('*', '&', '&&')),
    )),

    // Macro arguments such as `va_arg(ap, unsigned long)`. Reserved size modifiers cannot be
    // read as names, and like other type arguments any ordinary reading wins over this one.
    _sized_type_argument: $ => prec.dynamic(PREC.PAREN_DECLARATOR * 2, prec.right(seq(
      field('type', $.sized_type_specifier),
      repeat($.type_qualifier),
      field('declarator', optional(choice($.abstract_pointer_declarator, $.abstract_reference_declarator))),
    ))),

    destructor_name: $ => prec(1, seq('~', $.identifier)),

    compound_literal_expression: ($, original) => choice(
      original,
      seq(
        field('type', choice(
          $._class_name,
          $.primitive_type,
          seq(optional('typename'), $.splice_type_specifier))),
        field('value', $.initializer_list),
      ),
    ),

    dependent_identifier: $ => seq('template', $.template_function),
    dependent_field_identifier: $ => seq('template', $.template_method),
    dependent_type_identifier: $ => seq('template', $.template_type),

    _scope_resolution: $ => prec(1, seq(
      field('scope', optional(choice(
        $._namespace_identifier,
        $.template_type,
        $.decltype,
        $.splice_expression,
        $.splice_type_specifier,
        alias($.dependent_type_identifier, $.dependent_name),
      ))),
      '::',
    )),

    qualified_field_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_field_identifier, $.dependent_name),
        alias($.qualified_field_identifier, $.qualified_identifier),
        $.template_method,
        prec.dynamic(2, $._field_identifier),
      )),
    ),

    qualified_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_identifier, $.dependent_name),
        $.qualified_identifier,
        $.template_function,
        prec.dynamic(1, seq(optional('template'), $.identifier)),
        $.operator_name,
        $.destructor_name,
        $.pointer_type_declarator,
      )),
    ),

    qualified_type_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_type_identifier, $.dependent_name),
        alias($.qualified_type_identifier, $.qualified_identifier),
        $.template_type,
        prec.dynamic(1, $._type_identifier),
      )),
    ),

    qualified_operator_cast_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
        $.operator_cast,
      )),
    ),

    _assignment_left_expression: ($, original) => choice(
      original,
      $.qualified_identifier,
      $.user_defined_literal,
    ),

    assignment_expression: $ => prec.right(PREC.ASSIGNMENT, seq(
      field('left', $._assignment_left_expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', choice($.expression, $.initializer_list)),
    )),

    _assignment_expression_lhs: $ => seq(
      field('left', $.expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', choice($.expression, $.initializer_list)),
    ),

    // This prevents an ambiguity between fold expressions
    // and assignment expressions within parentheses.
    parenthesized_expression: ($, original) => choice(
      original,
      seq('(', alias($._assignment_expression_lhs, $.assignment_expression), ')'),
    ),

    reflect_expression: $ => prec.right(seq(
      '^^',
      choice(
        '::',
        $.expression,
        $.type_descriptor,
      ),
    )),

    splice_specifier: $ => seq( '[:', $.expression, ':]'),
    _splice_specialization_specifier: $ => seq($.splice_specifier, $.template_argument_list),

    splice_type_specifier: $ => prec.right(choice(
      $.splice_specifier,
      $._splice_specialization_specifier,
    )),

    splice_expression: $ => prec.right(choice(
      $.splice_specifier,
      seq('template', $._splice_specialization_specifier),
    )),

    expansion_statement: $ => seq(
      'template', 'for',
      '(',
      $._for_range_loop_body,
      ')',
      field('body', $.statement),
    ),

    operator_name: $ => prec(1, seq(
      'operator',
      choice(
        'co_await',
        '+', '-', '*', '/', '%',
        '^', '&', '|', '~',
        '!', '=', '<', '>',
        '+=', '-=', '*=', '/=', '%=', '^=', '&=', '|=',
        '<<', '>>', '>>=', '<<=',
        '==', '!=', '<=', '>=',
        '<=>',
        '&&', '||',
        '++', '--',
        ',',
        '->*',
        '->',
        '()', '[]',
        'xor', 'bitand', 'bitor', 'compl',
        'not', 'xor_eq', 'and_eq', 'or_eq', 'not_eq',
        'and', 'or',
        seq(choice('new', 'delete'), optional('[]')),
        seq('""', $.identifier),
      ),
    )),

    this: _ => 'this',

    // As in C, an unexpanded macro name may follow the first literal, e.g. `"%" PRIu64`.
    // After a string literal, a macro call such as `STRINGIFY(N)` can only expand to more of the
    // string, like the macro names upstream already admits here.
    concatenated_string: $ => prec.right(seq(
      choice(
        seq($.identifier, choice($.string_literal, $.raw_string_literal)),
        seq(choice($.string_literal, $.raw_string_literal), choice($.string_literal, $.raw_string_literal)),
        seq(choice($.string_literal, $.raw_string_literal), $.identifier),
        seq(choice($.string_literal, $.raw_string_literal), $._string_macro_call),
      ),
      repeat(choice($.identifier, $.string_literal, $.raw_string_literal, $._string_macro_call)),
    )),

    // `"a" M(x)` may also call a function named by `"a" M`, as in `operator"" _suffix(x)`; that
    // ordinary reading wins wherever both parse.
    _string_macro_call: $ => prec.dynamic(-1, alias(seq(
      field('function', $.identifier),
      field('arguments', $.argument_list),
    ), $.call_expression)),

    // Operands written next to each other without an operator, starting with a call, such as
    // `TEXT("a") TEXT("b")` or `DEFAULT_A(x) DEFAULT_B(y)`. Only unexpanded macros juxtapose like
    // this; whether they concatenate or select is unknown, so the region stays labeled. Every
    // ordinary reading, such as the casts in `(A)(B)c`, wins over this one.
    uncertain_concatenation: $ => prec.dynamic(-3, prec.right(PREC.CALL, seq(
      $.call_expression,
      repeat1(choice($.string_literal, $.raw_string_literal, $._juxtaposed_call)),
    ))),

    // The scanner certifies the callee only when balanced arguments follow it, and never a keyword.
    _juxtaposed_call: $ => alias(seq(
      field('function', alias($._juxtaposed_callee, $.identifier)),
      field('arguments', $.argument_list),
    ), $.call_expression),

    number_literal: $ => {
      const sign = /[-\+]/;
      const separator = '\'';
      const binary = /[01]/;
      const binaryDigits = seq(repeat1(binary), repeat(seq(separator, repeat1(binary))));
      const decimal = /[0-9]/;
      const firstDecimal = /[1-9]/;
      const intDecimalDigits = seq(firstDecimal, repeat(decimal), repeat(seq(separator, repeat1(decimal))));
      const floatDecimalDigits = seq(repeat1(decimal), repeat(seq(separator, repeat1(decimal))));
      const hex = /[0-9a-fA-F]/;
      const hexDigits = seq(repeat1(hex), repeat(seq(separator, repeat1(hex))));
      const octal = /[0-7]/;
      const octalDigits = seq('0', repeat(octal), repeat(seq(separator, repeat1(octal))));
      const hexExponent = seq(/[pP]/, optional(sign), floatDecimalDigits);
      const decimalExponent = seq(/[eE]/, optional(sign), floatDecimalDigits);
      const intSuffix = /(ll|LL)[uU]?|[uU](ll|LL)?|[uU][lL]?|[uU][zZ]?|[lL][uU]?|[zZ][uU]?/;
      const floatSuffix = /([fF](16|32|64|128)?)|[lL]|(bf16|BF16)/;

      return token(seq(
        optional(sign),
        choice(
          seq(
            choice(
              seq(choice('0b', '0B'), binaryDigits),
              intDecimalDigits,
              seq(choice('0x', '0X'), hexDigits),
              octalDigits,
            ),
            optional(intSuffix),
          ),
          seq(
            choice(
              seq(floatDecimalDigits, decimalExponent),
              seq(floatDecimalDigits, '.', optional(floatDecimalDigits), optional(decimalExponent)),
              seq('.', floatDecimalDigits, optional(decimalExponent)),
              seq(
                choice('0x', '0X'),
                choice(
                  hexDigits,
                  seq(hexDigits, '.', optional(hexDigits)),
                  seq('.', hexDigits)),
                hexExponent,
              ),
            ),
            optional(floatSuffix),
          ),
        ),
      ));
    },

    literal_suffix: _ => token.immediate(/[a-zA-Z_]\w*/),

    user_defined_literal: $ => seq(
      choice(
        $.number_literal,
        $.char_literal,
        $._string,
      ),
      $.literal_suffix,
    ),

    _namespace_identifier: $ => alias($.identifier, $.namespace_identifier),
  },
});
