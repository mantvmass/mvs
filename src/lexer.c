/*
 * lexer.c - ตัวตัดคำ (Tokenizer) ของภาษา MVS เขียนด้วยมือล้วน ๆ ไม่พึ่ง flex
 *
 * หลักการ: เดินอ่านซอร์สโค้ดทีละตัวอักษร แล้วจับกลุ่มเป็น token
 * - ข้ามช่องว่างและคอมเมนต์
 * - อ่านตัวเลข / สตริง / อักขระ / ชื่อ (identifier หรือ keyword)
 * - อ่านตัวดำเนินการและเครื่องหมายต่าง ๆ (รองรับตัวดำเนินการสองตัวอักษร เช่น ==, ->, ++)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

/* ตารางจับคู่คำสงวนกับชนิด token ใช้ค้นหาเมื่ออ่าน identifier เสร็จ */
typedef struct { const char *word; TokenType type; } Keyword;
static const Keyword KEYWORDS[] = {
    {"let", TK_LET}, {"const", TK_CONST}, {"func", TK_FUNC}, {"return", TK_RETURN},
    {"struct", TK_STRUCT}, {"if", TK_IF}, {"elseif", TK_ELSEIF}, {"else", TK_ELSE},
    {"while", TK_WHILE}, {"do", TK_DO}, {"for", TK_FOR}, {"switch", TK_SWITCH},
    {"case", TK_CASE}, {"default", TK_DEFAULT}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"import", TK_IMPORT}, {"from", TK_FROM},
    {"extern", TK_EXTERN}, {"export", TK_EXPORT}, {"impl", TK_IMPL}, {"trait", TK_TRAIT},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"as", TK_AS},
    /* ชนิดข้อมูล */
    {"i8", TK_TYPE_I8}, {"i16", TK_TYPE_I16}, {"i32", TK_TYPE_I32}, {"i64", TK_TYPE_I64},
    {"i128", TK_TYPE_I128}, {"isize", TK_TYPE_ISIZE},
    {"u8", TK_TYPE_U8}, {"u16", TK_TYPE_U16}, {"u32", TK_TYPE_U32}, {"u64", TK_TYPE_U64},
    {"u128", TK_TYPE_U128}, {"usize", TK_TYPE_USIZE},
    {"bool", TK_TYPE_BOOL}, {"void", TK_TYPE_VOID}, {"str", TK_TYPE_STR},
    {"char", TK_TYPE_CHAR}, {"f32", TK_TYPE_F32}, {"f64", TK_TYPE_F64},
    {NULL, TK_EOF}
};

/* เริ่มต้นสถานะ lexer */
void lexer_init(Lexer *lx, const char *src, const char *filename) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
    lx->filename = filename;
    lx->had_error = 0;
}

/* ดูตัวอักษรปัจจุบันโดยไม่ขยับตำแหน่ง */
static char peek(Lexer *lx) { return lx->src[lx->pos]; }

/* ดูตัวอักษรถัดไป (มองล่วงหน้า 1 ตัว) */
static char peek_next(Lexer *lx) {
    if (lx->src[lx->pos] == '\0') return '\0';
    return lx->src[lx->pos + 1];
}

/* กินตัวอักษรปัจจุบันแล้วขยับตำแหน่ง พร้อมอัปเดตบรรทัด/คอลัมน์ */
static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else { lx->col++; }
    return c;
}

/* สร้าง token พร้อมคัดลอกข้อความ (lexeme) ในช่วง [start, end) ของซอร์ส */
static Token make_token(Lexer *lx, TokenType type, size_t start, size_t end, int line, int col) {
    Token t;
    t.type = type;
    size_t len = end - start;
    t.lexeme = (char *)malloc(len + 1);
    memcpy(t.lexeme, lx->src + start, len);
    t.lexeme[len] = '\0';
    t.line = line;
    t.col = col;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}

/* สร้าง token แบบง่าย (ตัวดำเนินการ/เครื่องหมาย) ที่ระบุ lexeme ตายตัว */
static Token simple_token(TokenType type, const char *lex, int line, int col) {
    Token t;
    t.type = type;
    t.lexeme = strdup(lex);
    t.line = line;
    t.col = col;
    t.int_val = 0;
    t.float_val = 0.0;
    return t;
}

/* ข้ามช่องว่างและคอมเมนต์ทั้งแบบบรรทัดเดียวและหลายบรรทัด */
static void skip_trivia(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '/' && peek_next(lx) == '/') {
            /* คอมเมนต์บรรทัดเดียว: ข้ามจนจบบรรทัด */
            while (peek(lx) != '\n' && peek(lx) != '\0') advance(lx);
        } else if (c == '/' && peek_next(lx) == '*') {
            /* คอมเมนต์หลายบรรทัด: ข้ามจนกว่าจะเจอ *\/ */
            advance(lx); advance(lx);
            while (!(peek(lx) == '*' && peek_next(lx) == '/') && peek(lx) != '\0') advance(lx);
            if (peek(lx) != '\0') { advance(lx); advance(lx); } /* กิน *\/ */
        } else {
            break;
        }
    }
}

/* อ่าน identifier หรือ keyword: เริ่มด้วยตัวอักษร/_ ตามด้วยตัวอักษร/ตัวเลข/_ */
static Token read_identifier(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    /* จำกัดความยาว identifier กันบัฟเฟอร์ label/signature (สร้างจากชื่อ) ล้น (ดู func_label_of ฯลฯ) */
    if (lx->pos - start > 100) {
        lx->had_error = 1;
        fprintf(stderr, "%s:%d: error: identifier too long (max 100 characters)\n", lx->filename, line);
    }
    Token t = make_token(lx, TK_IDENT, start, lx->pos, line, col);
    /* ตรวจว่าตรงกับคำสงวนหรือไม่ ถ้าตรงให้เปลี่ยนชนิด */
    for (int i = 0; KEYWORDS[i].word != NULL; i++) {
        if (strcmp(t.lexeme, KEYWORDS[i].word) == 0) {
            t.type = KEYWORDS[i].type;
            break;
        }
    }
    return t;
}

/* อ่านตัวเลข: จำนวนเต็มหรือทศนิยม (มีจุด) */
static Token read_number(Lexer *lx) {
    int line = lx->line, col = lx->col;
    size_t start = lx->pos;
    int is_float = 0;
    while (isdigit((unsigned char)peek(lx))) advance(lx);
    /* ตรวจจุดทศนิยม โดยต้องตามด้วยตัวเลข เพื่อไม่ชนกับ member access (a.b) */
    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        is_float = 1;
        advance(lx); /* กินจุด */
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }
    Token t = make_token(lx, is_float ? TK_FLOAT : TK_INT, start, lx->pos, line, col);
    if (is_float) t.float_val = atof(t.lexeme);
    /* ใช้ strtoull เพื่อรองรับค่าจำนวนเต็มไม่มีเครื่องหมายเต็มช่วง 64-bit (เช่น u64 > 2^63)
     * เก็บเป็น bit-pattern ใน int_val (signed) — บิตยังถูกต้องเมื่อนำไป mov rax */
    else          t.int_val = (long long)strtoull(t.lexeme, NULL, 10);
    return t;
}

/* อ่านสตริงในเครื่องหมาย "..." รองรับ escape เบื้องต้น (\n \t \" \\ \0) */
static Token read_string(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* กิน " เปิด */
    /* เก็บค่าที่ถอด escape แล้วลงบัฟเฟอร์ใหม่ */
    char *buf = (char *)malloc(strlen(lx->src + lx->pos) + 1);
    size_t bi = 0;
    while (peek(lx) != '"' && peek(lx) != '\0') {
        char c = advance(lx);
        if (c == '\\') { /* จัดการ escape sequence */
            if (peek(lx) == '\0') break; /* '\' อยู่ท้ายไฟล์: กันอ่านเกิน buffer */
            char e = advance(lx);
            switch (e) {
                case 'n': buf[bi++] = '\n'; break;
                case 't': buf[bi++] = '\t'; break;
                case 'r': buf[bi++] = '\r'; break;
                case '0': buf[bi++] = '\0'; break;
                case '"': buf[bi++] = '"';  break;
                case '\\': buf[bi++] = '\\'; break;
                default:  buf[bi++] = e;    break;
            }
        } else {
            buf[bi++] = c;
        }
    }
    if (peek(lx) == '"') advance(lx); /* กิน " ปิด */
    else { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated string\n", lx->filename, line); }
    buf[bi] = '\0';
    Token t;
    t.type = TK_STRING;
    t.lexeme = buf; /* lexeme เก็บค่าที่ถอด escape แล้ว (ไม่รวมเครื่องหมายคำพูด) */
    t.line = line; t.col = col;
    t.int_val = (long long)bi; /* เก็บความยาวจริงไว้ใน int_val เผื่อมี \0 ภายใน */
    t.float_val = 0.0;
    return t;
}

/* อ่านอักขระเดี่ยวในเครื่องหมาย '...' */
static Token read_char(Lexer *lx) {
    int line = lx->line, col = lx->col;
    advance(lx); /* กิน ' เปิด */
    long long val = 0;
    if (peek(lx) == '\0') { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated char\n", lx->filename, line); return simple_token(TK_CHAR, "char", line, col); }
    char c = advance(lx);
    if (c == '\\') { /* escape เช่น '\n' */
        if (peek(lx) == '\0') { lx->had_error = 1; return simple_token(TK_CHAR, "char", line, col); } /* '\' ท้ายไฟล์ */
        char e = advance(lx);
        switch (e) {
            case 'n': val = '\n'; break;
            case 't': val = '\t'; break;
            case 'r': val = '\r'; break;
            case '0': val = '\0'; break;
            case '\\': val = '\\'; break;
            case '\'': val = '\''; break;
            default:  val = e;    break;
        }
    } else {
        val = (unsigned char)c;
    }
    if (peek(lx) == '\'') advance(lx); /* กิน ' ปิด */
    else { lx->had_error = 1; fprintf(stderr, "%s:%d: error: unterminated char\n", lx->filename, line); }
    Token t = simple_token(TK_CHAR, "char", line, col);
    t.int_val = val;
    return t;
}

/* ดึง token ตัวถัดไป - ฟังก์ชันหลักของ lexer */
Token lexer_next(Lexer *lx) {
    skip_trivia(lx);

    int line = lx->line, col = lx->col;
    char c = peek(lx);

    /* จบไฟล์ */
    if (c == '\0') return simple_token(TK_EOF, "<eof>", line, col);

    /* identifier / keyword */
    if (isalpha((unsigned char)c) || c == '_') return read_identifier(lx);
    /* ตัวเลข */
    if (isdigit((unsigned char)c)) return read_number(lx);
    /* สตริง */
    if (c == '"') return read_string(lx);
    /* อักขระ */
    if (c == '\'') return read_char(lx);

    /* ตัวดำเนินการและเครื่องหมาย: ตรวจแบบสองตัวอักษรก่อนเสมอ */
    char n = peek_next(lx);
    switch (c) {
        case '+':
            if (n == '+') { advance(lx); advance(lx); return simple_token(TK_PLUSPLUS, "++", line, col); }
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_PLUS_ASSIGN, "+=", line, col); }
            advance(lx); return simple_token(TK_PLUS, "+", line, col);
        case '-':
            if (n == '-') { advance(lx); advance(lx); return simple_token(TK_MINUSMINUS, "--", line, col); }
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_MINUS_ASSIGN, "-=", line, col); }
            if (n == '>') { advance(lx); advance(lx); return simple_token(TK_ARROW, "->", line, col); }
            advance(lx); return simple_token(TK_MINUS, "-", line, col);
        case '*':
            if (n == '*') { advance(lx); advance(lx); return simple_token(TK_STARSTAR, "**", line, col); } /* ยกกำลัง */
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_STAR_ASSIGN, "*=", line, col); }
            advance(lx); return simple_token(TK_STAR, "*", line, col);
        case '/':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_SLASH_ASSIGN, "/=", line, col); }
            advance(lx); return simple_token(TK_SLASH, "/", line, col);
        case '%': advance(lx); return simple_token(TK_PERCENT, "%", line, col);
        case '^': advance(lx); return simple_token(TK_CARET, "^", line, col);
        case '=':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_EQ, "==", line, col); }
            advance(lx); return simple_token(TK_ASSIGN, "=", line, col);
        case '!':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_NEQ, "!=", line, col); }
            advance(lx); return simple_token(TK_NOT, "!", line, col);
        case '<':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_LE, "<=", line, col); }
            if (n == '<') { advance(lx); advance(lx); return simple_token(TK_SHL, "<<", line, col); }
            advance(lx); return simple_token(TK_LT, "<", line, col);
        case '>':
            if (n == '=') { advance(lx); advance(lx); return simple_token(TK_GE, ">=", line, col); }
            if (n == '>') { advance(lx); advance(lx); return simple_token(TK_SHR, ">>", line, col); }
            advance(lx); return simple_token(TK_GT, ">", line, col);
        case '&':
            if (n == '&') { advance(lx); advance(lx); return simple_token(TK_AND, "&&", line, col); }
            advance(lx); return simple_token(TK_AMP, "&", line, col);
        case '~': advance(lx); return simple_token(TK_TILDE, "~", line, col);
        case '|':
            if (n == '|') { advance(lx); advance(lx); return simple_token(TK_OR, "||", line, col); }
            advance(lx); return simple_token(TK_PIPE, "|", line, col); /* bitwise OR */
        case '(': advance(lx); return simple_token(TK_LPAREN, "(", line, col);
        case ')': advance(lx); return simple_token(TK_RPAREN, ")", line, col);
        case '{': advance(lx); return simple_token(TK_LBRACE, "{", line, col);
        case '}': advance(lx); return simple_token(TK_RBRACE, "}", line, col);
        case '[': advance(lx); return simple_token(TK_LBRACKET, "[", line, col);
        case ']': advance(lx); return simple_token(TK_RBRACKET, "]", line, col);
        case ';': advance(lx); return simple_token(TK_SEMICOLON, ";", line, col);
        case ':':
            advance(lx);
            if (peek(lx) == ':') { advance(lx); return simple_token(TK_COLONCOLON, "::", line, col); } /* :: เรียก associated function */
            return simple_token(TK_COLON, ":", line, col);
        case ',': advance(lx); return simple_token(TK_COMMA, ",", line, col);
        case '.': advance(lx); return simple_token(TK_DOT, ".", line, col);
    }

    /* อักขระที่ไม่รู้จัก */
    lx->had_error = 1;
    fprintf(stderr, "%s:%d:%d: error: unexpected character '%c'\n", lx->filename, line, col, c);
    advance(lx);
    return simple_token(TK_ERROR, "?", line, col);
}
