/*
 * token.h - นิยามชนิดของ Token ทั้งหมดในภาษา MVS
 *
 * Token คือหน่วยเล็กที่สุดที่มีความหมายในโค้ด เช่น คำสงวน (keyword),
 * ตัวระบุชื่อ (identifier), ตัวเลข, สตริง, ตัวดำเนินการ ฯลฯ
 * ไฟล์นี้ใช้ร่วมกันระหว่าง lexer (ผู้สร้าง token) และ parser (ผู้บริโภค token)
 */
#ifndef MVS_TOKEN_H
#define MVS_TOKEN_H

/* ชนิดของ Token ทุกชนิดที่ lexer สามารถผลิตได้ */
typedef enum {
    /* --- กลุ่มทั่วไป / จบไฟล์ --- */
    TK_EOF = 0,      /* สิ้นสุดไฟล์ (End Of File) */
    TK_ERROR,        /* token ที่ผิดพลาด ใช้รายงาน error */

    /* --- ค่าคงที่ (literal) --- */
    TK_INT,          /* เลขจำนวนเต็ม เช่น 42 */
    TK_FLOAT,        /* เลขทศนิยม เช่น 3.14 */
    TK_STRING,       /* สตริง เช่น "hello" */
    TK_CHAR,         /* อักขระ เช่น 'A' */
    TK_IDENT,        /* ชื่อตัวแปร/ฟังก์ชัน เช่น myVar */

    /* --- คำสงวน (keyword) --- */
    TK_LET,          /* let     - ประกาศตัวแปรเปลี่ยนค่าได้ */
    TK_CONST,        /* const   - ประกาศค่าคงที่ */
    TK_FUNC,         /* func    - ประกาศฟังก์ชัน */
    TK_RETURN,       /* return  - คืนค่าและออกจากฟังก์ชัน */
    TK_STRUCT,       /* struct  - ประกาศโครงสร้างข้อมูล */
    TK_IF,           /* if      - เงื่อนไข */
    TK_ELSEIF,       /* elseif  - เงื่อนไขถัดไป */
    TK_ELSE,         /* else    - กรณีอื่น ๆ */
    TK_WHILE,        /* while   - ลูปตามเงื่อนไข */
    TK_DO,           /* do      - ลูปทำอย่างน้อยหนึ่งครั้ง */
    TK_FOR,          /* for     - ลูปนับรอบ */
    TK_SWITCH,       /* switch  - เลือกตามค่า */
    TK_CASE,         /* case    - กรณีใน switch */
    TK_DEFAULT,      /* default - กรณีปริยายใน switch */
    TK_BREAK,        /* break   - ออกจากลูป/switch */
    TK_CONTINUE,     /* continue- ข้ามไปรอบถัดไป */
    TK_IMPORT,       /* import  - นำเข้าโมดูล */
    TK_FROM,         /* from    - ระบุที่มาของ import */
    TK_EXTERN,       /* extern  - ประกาศฟังก์ชันภายนอก (foreign function จาก C runtime) */
    TK_EXPORT,       /* export  - ส่งออกฟังก์ชันให้เรียกจากภาษา C ได้ (ใช้ชื่อสัญลักษณ์ดิบ) */
    TK_IMPL,         /* impl    - บล็อกนิยาม method ของ struct (แบบ Rust) */
    TK_TRAIT,        /* trait   - นิยาม contract (ชุด method signature) แบบ Rust */
    TK_TRUE,         /* true    - ค่าบูลีนจริง */
    TK_FALSE,        /* false   - ค่าบูลีนเท็จ */
    TK_AS,           /* as      - แปลงชนิดอย่างชัดเจน (เช่น x as i32) */

    /* --- คำสงวนชนิดข้อมูล (type keyword) --- */
    TK_TYPE_I8, TK_TYPE_I16, TK_TYPE_I32, TK_TYPE_I64, TK_TYPE_I128, TK_TYPE_ISIZE,
    TK_TYPE_U8, TK_TYPE_U16, TK_TYPE_U32, TK_TYPE_U64, TK_TYPE_U128, TK_TYPE_USIZE,
    TK_TYPE_BOOL, TK_TYPE_VOID, TK_TYPE_STR, TK_TYPE_CHAR, TK_TYPE_F32, TK_TYPE_F64,

    /* --- ตัวดำเนินการทางคณิตศาสตร์ --- */
    TK_PLUS,         /* +  */
    TK_MINUS,        /* -  */
    TK_STAR,         /* *  (คูณ หรือ pointer/deref) */
    TK_SLASH,        /* /  */
    TK_PERCENT,      /* %  */
    TK_STARSTAR,     /* ** (ยกกำลัง) */

    /* --- ตัวดำเนินการเพิ่ม/ลดทีละหนึ่ง --- */
    TK_PLUSPLUS,     /* ++ */
    TK_MINUSMINUS,   /* -- */

    /* --- ตัวดำเนินการกำหนดค่า --- */
    TK_ASSIGN,       /* =  */
    TK_PLUS_ASSIGN,  /* += */
    TK_MINUS_ASSIGN, /* -= */
    TK_STAR_ASSIGN,  /* *= */
    TK_SLASH_ASSIGN, /* /= */

    /* --- ตัวดำเนินการเปรียบเทียบ --- */
    TK_EQ,           /* == */
    TK_NEQ,          /* != */
    TK_LT,           /* <  */
    TK_GT,           /* >  */
    TK_LE,           /* <= */
    TK_GE,           /* >= */

    /* --- ตัวดำเนินการตรรกะ --- */
    TK_AND,          /* && */
    TK_OR,           /* || */
    TK_NOT,          /* !  */
    TK_AMP,          /* &  (address-of หรือ bitwise AND ตามตำแหน่ง) */

    /* --- ตัวดำเนินการระดับบิต (bitwise) --- */
    TK_PIPE,         /* |  (bitwise OR) */
    TK_TILDE,        /* ~  (bitwise NOT) */
    TK_CARET,        /* ^  (bitwise XOR) */
    TK_SHL,          /* << (เลื่อนบิตซ้าย) */
    TK_SHR,          /* >> (เลื่อนบิตขวา) */

    /* --- เครื่องหมายวรรคตอน / โครงสร้าง --- */
    TK_ARROW,        /* -> (ระบุชนิดที่ฟังก์ชันคืนค่า) */
    TK_LPAREN,       /* (  */
    TK_RPAREN,       /* )  */
    TK_LBRACE,       /* {  */
    TK_RBRACE,       /* }  */
    TK_LBRACKET,     /* [  */
    TK_RBRACKET,     /* ]  */
    TK_SEMICOLON,    /* ;  */
    TK_COLON,        /* :  */
    TK_COLONCOLON,   /* :: (เรียก associated function เช่น Point::new) */
    TK_COMMA,        /* ,  */
    TK_DOT           /* .  */
} TokenType;

/* โครงสร้างของ Token หนึ่งตัว */
typedef struct {
    TokenType type;  /* ชนิดของ token */
    char     *lexeme;/* ข้อความดิบของ token (เช่น "42", "myVar") เป็นสำเนา (malloc) */
    int       line;  /* บรรทัดที่พบ token ใช้รายงาน error */
    int       col;   /* คอลัมน์ที่พบ token */

    /* ค่าที่แปลงแล้ว ใช้ตอนเป็น literal */
    long long int_val;   /* ค่าตัวเลขจำนวนเต็ม เมื่อ type == TK_INT */
    double    float_val; /* ค่าทศนิยม เมื่อ type == TK_FLOAT */
} Token;

#endif /* MVS_TOKEN_H */
