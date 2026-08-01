/**
 * Tree-sitter grammar for MVS, covering the syntax in docs/guide.md section 4.
 *
 * This is the editor-side parser, used only for syntax highlighting and
 * structural editing. The compiler's own parser in src/parser.c stays the
 * single source of truth for the language; where the two disagree, fix this
 * one. The grammar is deliberately a little permissive (it accepts some
 * programs the compiler rejects), because an editor grammar must keep
 * producing a usable tree while the user is mid-keystroke.
 */

const PREC = {
  ASSIGN: 1,
  OR: 2,
  AND: 3,
  BIT_OR: 4,
  BIT_XOR: 5,
  BIT_AND: 6,
  EQUALITY: 7,
  COMPARE: 8,
  SHIFT: 9,
  ADD: 10,
  MULTIPLY: 11,
  UNARY: 12,
  POWER: 13, // ** binds tighter than unary minus: -2 ** 2 is -(2 ** 2)
  CAST: 14,
  POSTFIX: 15,
};

function commaSep(rule) {
  return optional(commaSep1(rule));
}

// Trailing commas are legal in MVS (struct literals, array literals, calls),
// so every comma-separated list accepts one.
function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}

module.exports = grammar({
  name: 'mvs',

  extras: $ => [/\s/, $.line_comment, $.block_comment],

  word: $ => $.identifier,

  // The classic angle-bracket ambiguity: after `name <` the parser cannot
  // know locally whether this is a comparison or the start of Vec<i64>.
  // GLR explores both; dynamic precedence on the generic rules picks the
  // generic reading whenever it parses to completion.
  conflicts: $ => [
    [$._expression, $.generic_expression],
    [$.match_statement, $._expression],
    [$._type, $.generic_type],
  ],

  rules: {
    source_file: $ => repeat($._top_level_item),

    _top_level_item: $ => choice(
      $.attribute,
      $.import_statement,
      $.extern_declaration,
      $.function_definition,
      $.struct_definition,
      $.enum_definition,
      $.trait_definition,
      $.impl_block,
      $.let_declaration,
      $.const_declaration,
    ),

    // ---- attributes: @compile(target_os = "linux"), @test -----------------

    attribute: $ => seq(
      '@',
      field('name', $.identifier),
      optional(seq('(', commaSep($.attribute_argument), ')')),
    ),

    attribute_argument: $ => seq(
      field('key', $.identifier),
      '=',
      field('value', $.string_literal),
    ),

    // ---- imports: the three forms (the path decides) -----------------------

    import_statement: $ => choice(
      seq('import', '{', commaSep1($.identifier), '}', 'from', field('path', $.string_literal), ';'),
      seq('import', field('alias', $.identifier), 'from', field('path', $.string_literal), ';'),
    ),

    // ---- functions ---------------------------------------------------------

    function_definition: $ => seq(
      optional('export'),
      'func',
      field('name', $.identifier),
      optional($.type_parameters),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      optional($.where_clause),
      field('body', $.block),
    ),

    extern_declaration: $ => seq(
      'extern', 'func',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      ';',
    ),

    // A signature inside a trait body: no block, just a semicolon.
    function_signature: $ => seq(
      'func',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      optional(seq('->', field('return_type', $._type))),
      ';',
    ),

    parameter_list: $ => seq('(', commaSep($.parameter), ')'),

    parameter: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', choice($._type, $.variadic_type)),
      optional(seq('=', field('default', $._expression))),
    ),

    type_parameters: $ => seq('<', commaSep1($.type_parameter), '>'),

    type_parameter: $ => seq(
      field('name', alias($.identifier, $.type_identifier)),
      optional(seq(':', $.trait_bound)),
    ),

    trait_bound: $ => seq(
      alias($.identifier, $.type_identifier),
      repeat(seq('+', alias($.identifier, $.type_identifier))),
    ),

    where_clause: $ => seq(
      'where',
      commaSep1(seq(
        alias($.identifier, $.type_identifier),
        ':',
        $.trait_bound,
      )),
    ),

    // ---- struct / enum / trait / impl -------------------------------------

    struct_definition: $ => seq(
      'struct',
      field('name', alias($.identifier, $.type_identifier)),
      optional($.type_parameters),
      '{',
      repeat($.field_declaration),
      '}',
    ),

    // Fields may be separated by ; or , and the last separator is optional.
    field_declaration: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
      optional(choice(';', ',')),
    ),

    enum_definition: $ => seq(
      'enum',
      field('name', alias($.identifier, $.type_identifier)),
      optional($.type_parameters),
      '{',
      commaSep($.enum_variant),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq('(', commaSep1($._type), ')')),
    ),

    trait_definition: $ => seq(
      'trait',
      field('name', alias($.identifier, $.type_identifier)),
      '{',
      repeat(choice($.function_signature, $.function_definition)),
      '}',
    ),

    impl_block: $ => seq(
      'impl',
      choice(
        seq(field('trait', $._impl_type), 'for', field('type', $._impl_type)),
        field('type', $._impl_type),
      ),
      '{',
      repeat($.function_definition),
      '}',
    ),

    _impl_type: $ => choice(
      $.primitive_type,
      alias($.identifier, $.type_identifier),
      $.generic_type,
    ),

    // ---- types -------------------------------------------------------------

    _type: $ => choice(
      $.primitive_type,
      alias($.identifier, $.type_identifier),
      $.generic_type,
      $.pointer_type,
      $.array_type,
      $.function_type,
      $.dyn_type,
    ),

    primitive_type: _ => choice(
      'i8', 'i16', 'i32', 'i64', 'i128',
      'u8', 'u16', 'u32', 'u64', 'u128',
      'isize', 'usize', 'bool', 'char', 'str', 'f32', 'f64', 'void',
    ),

    generic_type: $ => seq(
      field('name', alias($.identifier, $.type_identifier)),
      $.type_arguments,
    ),

    type_arguments: $ => seq('<', commaSep1($._type), '>'),

    pointer_type: $ => seq('*', $._type),

    array_type: $ => seq('[', $._type, ';', $._expression, ']'),

    function_type: $ => seq(
      'func', '(', commaSep($._type), ')',
      optional(seq('->', $._type)),
    ),

    dyn_type: $ => seq('dyn', alias($.identifier, $.type_identifier)),

    // Only valid as the last parameter's type: items: ...dyn Shape
    variadic_type: $ => seq('...', $.dyn_type),

    // ---- statements --------------------------------------------------------

    block: $ => seq('{', repeat($._statement), '}'),

    _statement: $ => choice(
      $.let_declaration,
      $.const_declaration,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.do_while_statement,
      $.switch_statement,
      $.match_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.block,
      $.expression_statement,
      $.empty_statement,
    ),

    let_declaration: $ => seq(
      'let',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
      ';',
    ),

    const_declaration: $ => seq(
      'const',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
      ';',
    ),

    if_statement: $ => seq(
      'if', '(', field('condition', $._expression), ')',
      field('consequence', $.block),
      repeat($.elseif_clause),
      optional($.else_clause),
    ),

    // `elseif` is one keyword; `else if` chains through else_clause instead.
    elseif_clause: $ => seq(
      'elseif', '(', field('condition', $._expression), ')',
      field('consequence', $.block),
    ),

    else_clause: $ => seq('else', choice($.block, $.if_statement)),

    while_statement: $ => seq(
      'while', '(', field('condition', $._expression), ')',
      field('body', $.block),
    ),

    for_statement: $ => seq(
      'for', '(',
      field('initializer', choice($.let_declaration, $.expression_statement, ';')),
      optional(field('condition', $._expression)), ';',
      optional(field('update', $._expression)),
      ')',
      field('body', $.block),
    ),

    do_while_statement: $ => seq(
      'do', field('body', $.block),
      'while', '(', field('condition', $._expression), ')', ';',
    ),

    switch_statement: $ => seq(
      'switch', '(', field('value', $._expression), ')',
      '{',
      repeat(choice($.switch_case, $.default_case)),
      '}',
    ),

    switch_case: $ => seq(
      'case', field('value', $._expression), ':',
      repeat($._statement),
    ),

    default_case: $ => seq('default', ':', repeat($._statement)),

    // A match used as a statement carries no trailing semicolon.
    match_statement: $ => $.match_expression,

    return_statement: $ => seq('return', optional($._expression), ';'),

    break_statement: _ => seq('break', ';'),

    continue_statement: _ => seq('continue', ';'),

    expression_statement: $ => seq($._expression, ';'),

    empty_statement: _ => ';',

    // ---- match -------------------------------------------------------------

    match_expression: $ => seq(
      'match', '(', field('value', $._expression), ')',
      '{',
      repeat($.match_arm),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $.pattern),
      '=>',
      field('value', choice($.block, $._expression)),
      optional(','),
    ),

    // Patterns are one level deep in MVS v1: a variant name, optionally
    // qualified with the enum, optionally binding its payload by position.
    pattern: $ => choice(
      $.wildcard_pattern,
      seq(
        optional(seq(field('enum', alias($.identifier, $.type_identifier)), '::')),
        field('variant', $.identifier),
        optional(seq('(', commaSep($.identifier), ')')),
      ),
    ),

    wildcard_pattern: _ => '_',

    // ---- expressions -------------------------------------------------------

    _expression: $ => choice(
      $.identifier,
      $.integer_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.boolean_literal,
      $.array_literal,
      $.struct_literal,
      $.unary_expression,
      $.binary_expression,
      $.assignment_expression,
      $.update_expression,
      $.cast_expression,
      $.call_expression,
      $.index_expression,
      $.member_expression,
      $.path_expression,
      $._generic_head,
      $.match_expression,
      $.parenthesized_expression,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    // `Vec<i64>` before ::, ( or {. On its own it is never a complete
    // expression, but modeling it as one keeps call/path/struct-literal
    // rules simple; GLR + dynamic precedence sort out `a < b > (c)`.
    _generic_head: $ => prec.dynamic(1, $.generic_expression),

    generic_expression: $ => seq(
      field('name', $.identifier),
      $.type_arguments,
    ),

    call_expression: $ => prec(PREC.POSTFIX, seq(
      field('function', $._expression),
      field('arguments', $.argument_list),
    )),

    argument_list: $ => seq('(', commaSep($._expression), ')'),

    index_expression: $ => prec(PREC.POSTFIX, seq(
      field('value', $._expression),
      '[', field('index', $._expression), ']',
    )),

    member_expression: $ => prec(PREC.POSTFIX, seq(
      field('value', $._expression),
      '.',
      field('member', $.identifier),
    )),

    // Type::assoc, module::func, Enum::Variant, Vec<i64>::new. Paths chain.
    path_expression: $ => prec(PREC.POSTFIX, seq(
      field('path', choice(
        $.identifier,
        $.primitive_type,
        $._generic_head,
        $.path_expression,
      )),
      '::',
      field('name', $.identifier),
    )),

    struct_literal: $ => seq(
      field('name', choice(
        alias($.identifier, $.type_identifier),
        $._generic_head,
      )),
      '{',
      commaSep($.field_initializer),
      '}',
    ),

    field_initializer: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    array_literal: $ => seq('[', commaSep($._expression), ']'),

    unary_expression: $ => prec(PREC.UNARY, seq(
      field('operator', choice('-', '!', '~', '*', '&')),
      field('operand', $._expression),
    )),

    update_expression: $ => prec(PREC.POSTFIX, seq(
      field('operand', $._expression),
      field('operator', choice('++', '--')),
    )),

    cast_expression: $ => prec.left(PREC.CAST, seq(
      field('value', $._expression),
      'as',
      field('type', $._type),
    )),

    assignment_expression: $ => prec.right(PREC.ASSIGN, seq(
      field('left', $._expression),
      field('operator', choice('=', '+=', '-=', '*=', '/=')),
      field('right', $._expression),
    )),

    binary_expression: $ => {
      const table = [
        ['||', PREC.OR],
        ['&&', PREC.AND],
        ['|', PREC.BIT_OR],
        ['^', PREC.BIT_XOR],
        ['&', PREC.BIT_AND],
        ['==', PREC.EQUALITY], ['!=', PREC.EQUALITY],
        ['<', PREC.COMPARE], ['>', PREC.COMPARE],
        ['<=', PREC.COMPARE], ['>=', PREC.COMPARE],
        ['<<', PREC.SHIFT], ['>>', PREC.SHIFT],
        ['+', PREC.ADD], ['-', PREC.ADD],
        ['*', PREC.MULTIPLY], ['/', PREC.MULTIPLY], ['%', PREC.MULTIPLY],
      ];
      return choice(
        ...table.map(([op, p]) => prec.left(p, seq(
          field('left', $._expression),
          field('operator', op),
          field('right', $._expression),
        ))),
        // Power is right-associative and binds tighter than unary minus.
        prec.right(PREC.POWER, seq(
          field('left', $._expression),
          field('operator', '**'),
          field('right', $._expression),
        )),
      );
    },

    // ---- lexical -----------------------------------------------------------

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    integer_literal: _ => token(choice(
      /0x[0-9a-fA-F]+/,
      /0b[01]+/,
      /[0-9]+/,
    )),

    // 3.14, 1e9, 2.5e-3, 6.02E23
    float_literal: _ => token(choice(
      /[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/,
      /[0-9]+[eE][+-]?[0-9]+/,
    )),

    boolean_literal: _ => choice('true', 'false'),

    // A backslash at end of line splices the line (the compiler supports it),
    // so an escape may be followed by a newline as well as any character.
    string_literal: _ => token(seq(
      '"',
      repeat(choice(/[^"\\\n]/, /\\(\r\n|\n|.)/)),
      '"',
    )),

    char_literal: _ => token(seq(
      "'",
      choice(/[^'\\\n]/, /\\./),
      "'",
    )),

    line_comment: _ => token(seq('//', /[^\n]*/)),

    // Block comments do not nest (the compiler warns on a nested /*).
    block_comment: _ => token(seq('/*', /([^*]|\*+[^*/])*\*+/, '/')),
  },
});
