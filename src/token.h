/*
 * token.h - Definitions of all Token kinds in the MVS language
 *
 * A token is the smallest meaningful unit in the code, e.g. a keyword,
 * an identifier, a number, a string, an operator, etc.
 * This file is shared between the lexer (token producer) and the parser (token consumer).
 */
#ifndef MVS_TOKEN_H
#define MVS_TOKEN_H

/* Every kind of token the lexer can produce */
typedef enum {
    /* --- General group / end of file --- */
    TK_EOF = 0,      /* End Of File */
    TK_ERROR,        /* invalid token, used for error reporting */

    /* --- Literals --- */
    TK_INT,          /* integer literal, e.g. 42 */
    TK_FLOAT,        /* floating-point literal, e.g. 3.14 */
    TK_STRING,       /* string literal, e.g. "hello" */
    TK_CHAR,         /* character literal, e.g. 'A' */
    TK_IDENT,        /* variable/function name, e.g. myVar */

    /* --- Keywords --- */
    TK_LET,          /* let     - declare a mutable variable */
    TK_CONST,        /* const   - declare a constant */
    TK_FUNC,         /* func    - declare a function */
    TK_RETURN,       /* return  - return a value and exit the function */
    TK_STRUCT,       /* struct  - declare a data structure */
    TK_IF,           /* if      - conditional */
    TK_ELSEIF,       /* elseif  - next conditional branch */
    TK_ELSE,         /* else    - fallback branch */
    TK_WHILE,        /* while   - conditional loop */
    TK_DO,           /* do      - loop that runs at least once */
    TK_FOR,          /* for     - counted loop */
    TK_SWITCH,       /* switch  - select by value */
    TK_CASE,         /* case    - a case inside switch */
    TK_DEFAULT,      /* default - default case inside switch */
    TK_BREAK,        /* break   - exit a loop/switch */
    TK_CONTINUE,     /* continue- skip to the next iteration */
    TK_IMPORT,       /* import  - import a module */
    TK_FROM,         /* from    - specifies the source of an import */
    TK_EXTERN,       /* extern  - declare an external function (foreign function from the C runtime) */
    TK_EXPORT,       /* export  - export a function so C can call it (uses the raw symbol name) */
    TK_IMPL,         /* impl    - block defining methods of a struct (Rust-style) */
    TK_TRAIT,        /* trait   - define a contract (set of method signatures), Rust-style */
    TK_ENUM,         /* enum    - a tagged sum type with payload variants (Rust-style) */
    TK_MATCH,        /* match   - branch on an enum's variant, binding its payload */
    TK_DYN,          /* dyn     - trait object type (dyn Trait = fat pointer {data, vtable}) */
    TK_WHERE,        /* where   - trait bound clause after the signature (where T: A + B) */
    TK_ELLIPSIS,     /* ...     - variadic parameter marker (args: ...dyn Display) */
    TK_TRUE,         /* true    - boolean true */
    TK_FALSE,        /* false   - boolean false */
    TK_AS,           /* as      - explicit type cast (e.g. x as i32) */

    /* --- Data type keywords --- */
    TK_TYPE_I8, TK_TYPE_I16, TK_TYPE_I32, TK_TYPE_I64, TK_TYPE_I128, TK_TYPE_ISIZE,
    TK_TYPE_U8, TK_TYPE_U16, TK_TYPE_U32, TK_TYPE_U64, TK_TYPE_U128, TK_TYPE_USIZE,
    TK_TYPE_BOOL, TK_TYPE_VOID, TK_TYPE_STR, TK_TYPE_CHAR, TK_TYPE_F32, TK_TYPE_F64,

    /* --- Arithmetic operators --- */
    TK_PLUS,         /* +  */
    TK_MINUS,        /* -  */
    TK_STAR,         /* *  (multiply, or pointer/deref) */
    TK_SLASH,        /* /  */
    TK_PERCENT,      /* %  */
    TK_STARSTAR,     /* ** (exponentiation) */

    /* --- Increment/decrement operators --- */
    TK_PLUSPLUS,     /* ++ */
    TK_MINUSMINUS,   /* -- */

    /* --- Assignment operators --- */
    TK_ASSIGN,       /* =  */
    TK_PLUS_ASSIGN,  /* += */
    TK_MINUS_ASSIGN, /* -= */
    TK_STAR_ASSIGN,  /* *= */
    TK_SLASH_ASSIGN, /* /= */

    /* --- Comparison operators --- */
    TK_EQ,           /* == */
    TK_NEQ,          /* != */
    TK_LT,           /* <  */
    TK_GT,           /* >  */
    TK_LE,           /* <= */
    TK_GE,           /* >= */

    /* --- Logical operators --- */
    TK_AND,          /* && */
    TK_OR,           /* || */
    TK_NOT,          /* !  */
    TK_AMP,          /* &  (address-of or bitwise AND, depending on position) */

    /* --- Bitwise operators --- */
    TK_PIPE,         /* |  (bitwise OR) */
    TK_TILDE,        /* ~  (bitwise NOT) */
    TK_CARET,        /* ^  (bitwise XOR) */
    TK_SHL,          /* << (shift left) */
    TK_SHR,          /* >> (shift right) */

    /* --- Punctuation / structure --- */
    TK_ARROW,        /* -> (specifies a function's return type) */
    TK_FATARROW,     /* => (separates a match arm's pattern from its body) */
    TK_LPAREN,       /* (  */
    TK_RPAREN,       /* )  */
    TK_LBRACE,       /* {  */
    TK_RBRACE,       /* }  */
    TK_LBRACKET,     /* [  */
    TK_RBRACKET,     /* ]  */
    TK_SEMICOLON,    /* ;  */
    TK_COLON,        /* :  */
    TK_COLONCOLON,   /* :: (associated function call, e.g. Point::new) */
    TK_COMMA,        /* ,  */
    TK_DOT,          /* .  */
    TK_AT            /* @  (attribute marker, e.g. @compile(target_os = "linux")) */
} TokenType;

/* Structure of a single Token */
typedef struct {
    TokenType type;  /* kind of token */
    char     *lexeme;/* raw text of the token (e.g. "42", "myVar"), a malloc'd copy */
    int       line;  /* line where the token was found, used for error reporting */
    int       col;   /* column where the token was found */

    /* Converted values, used when the token is a literal */
    long long int_val;   /* integer value when type == TK_INT */
    double    float_val; /* floating-point value when type == TK_FLOAT */
} Token;

#endif /* MVS_TOKEN_H */
