#pragma once
#include <stdint.h>
#ifndef NULL
#define NULL ((void*)0)
#endif
#include "../../userspace/libc/syscall.h"

/*  Platform  */

/* strlen alias (libc provides it) */
/* strcmp_u, memset_u, puts already defined in syscall.h */

/* memcpy — not in libc, provide inline */
static inline void *_ys_memcpy(void *dst, const void *src, int n){
    char *d=(char*)dst; const char *s=(const char*)src;
    while(n--)*d++=*s++;
    return dst;
}
#define memcpy _ys_memcpy

/* strncmp — not in libc, provide inline */
static inline int _ys_strncmp(const char *a,const char *b,int n){
    for(int i=0;i<n;i++){
        if(!a[i]||a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i];
    }
    return 0;
}
#define strncmp _ys_strncmp

/* stderr helper — writes to fd 2 */
static inline void ys_stderr(const char *s){
    write(2,s,(int)strlen(s));
}

/* ys_print — print without newline (same as puts from libc but explicit) */
static inline void ys_print(const char *s){ write(STDOUT_FILENO,s,(int)strlen(s)); }

/* Capability permissions */
#define CAP_READ  1
#define CAP_WRITE 2
#define CAP_EXEC  4

/*  Kernel capability helpers  */

/*
 * ys_cap_create — mint a BLAKE3 capability token via SYS_CAP_CREATE.
 *
 * Syscall ABI (SYS_CAP_CREATE = 10):
 *   in : rdi=res, rsi=res_id, rdx=rights, r10=owner_pid (0 = current)
 *   out: rax=tok.upper, rdi=tok.lower (written back into frame by kernel)
 *
 * Returns zero token on hardware fault (degrades gracefully to fd-only cap).
 */
static inline cap_token_t ys_cap_create(uint64_t res,uint64_t res_id,uint64_t rights){
    uint64_t upper=(uint64_t)SYS_CAP_CREATE;
    uint64_t lower=res;
    register uint64_t r10 __asm__("r10")=0;
    __asm__ volatile(
        "syscall"
        :"+a"(upper),"+D"(lower)
        :"S"(res_id),"d"(rights),"r"(r10)
        :"rcx","r11","r8","r9","memory"
    );
    cap_token_t tok={upper,lower};
    return tok;
}

/*
 * ys_cap_revoke — self-revoke a capability token via SYS_CAP_REVOKE.
 *
 * Syscall ABI (SYS_CAP_REVOKE = 12):
 *   rdi=auth.upper, rsi=auth.lower, rdx=tgt.upper, r10=tgt.lower
 */
static inline int ys_cap_revoke(uint64_t tok_upper,uint64_t tok_lower){
    int64_t ret;
    register uint64_t r10 __asm__("r10")=tok_lower;
    __asm__ volatile(
        "syscall"
        :"=a"(ret)
        :"0"((uint64_t)SYS_CAP_REVOKE),
         "D"(tok_upper),"S"(tok_lower),
         "d"(tok_upper),"r"(r10)
        :"rcx","r11","memory"
    );
    return (int)ret;
}

/*  Token types  */
typedef enum {
    TK_EOF, TK_NL,
    TK_INT, TK_FLOAT, TK_STR, TK_IDENT, TK_BOOL,
    TK_FN, TK_LET, TK_VAR, TK_IF, TK_ELSE, TK_WHILE,
    TK_FOR, TK_IN, TK_RETURN, TK_STRUCT, TK_IMPL,
    TK_MATCH, TK_UNSAFE, TK_TRUE, TK_FALSE, TK_IMPORT,
    TK_TRY, TK_CATCH, TK_THROW, TK_AS,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LTE, TK_GTE,
    TK_AND, TK_OR, TK_NOT, TK_ARROW, TK_FAT_ARROW, TK_DOTDOT, TK_DOT,
    TK_AMP, TK_PIPE, TK_CARET, TK_BANG,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET, TK_COMMA, TK_COLON,
    TK_SEMICOLON, TK_AT,
} TokenKind;

typedef struct {
    TokenKind   kind;
    const char *start;
    int         len;
    int64_t     ival;
    int64_t     fval;
    int         line;
} Token;

/*  AST node types  */
typedef enum {
    ND_INT, ND_FLOAT, ND_STR, ND_BOOL, ND_IDENT,
    ND_BINOP, ND_UNOP, ND_ASSIGN, ND_LET, ND_VAR,
    ND_CALL, ND_DOT, ND_INDEX, ND_INDEX_SET,
    ND_IF, ND_WHILE, ND_FOR, ND_RETURN, ND_BLOCK,
    ND_FN, ND_FN_LIT, ND_STRUCT, ND_STRUCT_LIT, ND_MATCH, ND_IMPORT,
    ND_TRY, ND_THROW, ND_MODULE,
    ND_ARRAY,
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind  kind;
    int64_t   ival;
    int64_t   fval;
    char      sval[256];
    int       op;
    Node     *left;
    Node     *right;
    Node     *cond;
    Node     *then;
    Node     *els;
    Node     *body;
    Node     *arg_data[16];
    Node    **args;
    int       argc;
    Node    **stmts;
    int       stmtc;
    char      name[64];
    char      type[32];
    int       is_mut;
    char      field_names[8][32];
};

/*  Value  */
typedef struct Val Val;

#define MAX_ARR 64
struct Val {
    int      type;
    int64_t  ival;
    int64_t  fval;
    int      bval;
    char     sval[256];
    Node    *fn_node;
    void    *fn_env;
    /* capability */
    int64_t  cap_fd;          /* kernel fd (int cast) */
    int      cap_perm;
    char     cap_path[128];
    uint64_t cap_tok_upper;   /* BLAKE3 kernel token — 0 if not minted */
    uint64_t cap_tok_lower;
    /* array */
    Val     *arr_data;
    int      arr_len;
    /* struct */
    char     struct_name[32];
    Val     *field_vals;
    char   (*field_names)[32];
    int      field_count;
};

/* Value type constants */
#define VT_NIL    0
#define VT_INT    1
#define VT_FLOAT  2
#define VT_BOOL   3
#define VT_STR    4
#define VT_FN     5
#define VT_CAP    6
#define VT_ARR    7
#define VT_STRUCT 8
#define VT_ERR    9

/*  Environment  */
#define ENV_MAX 32
typedef struct Env Env;
struct Env {
    char  names[ENV_MAX][64];
    Val   vals [ENV_MAX];
    int   count;
    Env  *parent;
};

/*  Lexer  */
typedef struct {
    const char *src;
    int         pos;
    int         len;
    Token       cur;
    int         line;
} Lexer;

/*  Function prototypes  */
void  lex_init      (Lexer *l, const char *src, int len);
Token lex_next      (Lexer *l);
Node *parse_program (Lexer *l);
Val   eval_program  (Node *prog, Env *env);
Val   eval_node     (Node *n,    Env *env);
Env  *env_new       (Env *parent);
Val  *env_get       (Env *e, const char *name);
void  env_set       (Env *e, const char *name, Val v);
void  env_def       (Env *e, const char *name, Val v);
void  env_free      (Env *e);
void  ys_print_val  (Val v);
void  ys_error      (int line, const char *msg);
void  parser_pool_save    (void);
void  parser_pool_restore (void);

extern char g_src_dir[512];