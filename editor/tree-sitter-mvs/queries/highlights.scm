; Highlight captures for MVS, following the nvim-treesitter capture names
; (which Helix and Zed also understand).

; ---- comments and literals ----

(line_comment) @comment
(block_comment) @comment

(string_literal) @string
(char_literal) @character
(integer_literal) @number
(float_literal) @number.float
(boolean_literal) @boolean

; ---- keywords ----

[
  "func"
  "struct"
  "enum"
  "trait"
  "impl"
  "let"
  "const"
  "extern"
  "export"
  "dyn"
  "where"
] @keyword

[
  "if"
  "else"
  "elseif"
  "switch"
  "case"
  "default"
  "match"
] @keyword.conditional

[
  "while"
  "for"
  "do"
  "break"
  "continue"
] @keyword.repeat

"return" @keyword.return

[
  "import"
  "from"
] @keyword.import

"as" @keyword.operator

; ---- types ----

(primitive_type) @type.builtin
(type_identifier) @type
(dyn_type (type_identifier) @type)

((type_identifier) @type.builtin
  (#eq? @type.builtin "Self"))

; ---- functions ----

(function_definition name: (identifier) @function)
(function_signature name: (identifier) @function)
(extern_declaration name: (identifier) @function)

(call_expression
  function: (identifier) @function.call)
(call_expression
  function: (member_expression member: (identifier) @function.method.call))
(call_expression
  function: (path_expression name: (identifier) @function.call))
(call_expression
  function: (generic_expression name: (identifier) @function.call))

; compiler intrinsics
((call_expression function: (identifier) @function.builtin)
  (#eq? @function.builtin "asm"))

; ---- structs, enums, paths ----

(struct_literal name: (type_identifier) @type)
(struct_literal name: (generic_expression name: (identifier) @type))
(generic_expression name: (identifier) @type)

; Enum::Variant and Type::assoc paths: a capitalized path segment is a type
(path_expression path: (identifier) @type
  (#match? @type "^[A-Z]"))
(path_expression name: (identifier) @constructor
  (#match? @constructor "^[A-Z]"))

; match patterns
(pattern enum: (type_identifier) @type)
(pattern variant: (identifier) @constructor
  (#match? @constructor "^[A-Z]"))
(wildcard_pattern) @constant.builtin

; ---- fields, parameters, variables ----

(field_declaration name: (identifier) @property)
(field_initializer name: (identifier) @property)
(member_expression member: (identifier) @property)

(parameter name: (identifier) @variable.parameter)
(type_parameter name: (type_identifier) @type)

((identifier) @variable.builtin
  (#eq? @variable.builtin "self"))

; SCREAMING_CASE names are constants
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))

; ---- attributes and modules ----

(attribute) @attribute
(attribute name: (identifier) @attribute)

(import_statement alias: (identifier) @module)

; ---- operators and punctuation ----

[
  "+" "-" "*" "/" "%" "**"
  "=" "+=" "-=" "*=" "/="
  "==" "!=" "<" ">" "<=" ">="
  "&&" "||" "!"
  "&" "|" "^" "~" "<<" ">>"
  "++" "--"
  "->" "=>"
  "..."
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
[";" ":" "," "." "::"] @punctuation.delimiter
"@" @punctuation.special
