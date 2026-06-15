#include "yolish.h"

static int is_alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int is_digit(char c){return c>='0'&&c<='9';}
static int is_alnum(char c){return is_alpha(c)||is_digit(c);}

static int streq(const char *a, int n, const char *b){
    int i=0; while(i<n&&b[i]&&a[i]==b[i])i++;
    return i==n&&!b[i];
}

static Token make_kw_or_ident(const char *s, int n){
    Token t; t.start=s; t.len=n; t.ival=0; t.fval=0;
    if(streq(s,n,"fn"))     {t.kind=TK_FN;     return t;}
    if(streq(s,n,"let"))    {t.kind=TK_LET;    return t;}
    if(streq(s,n,"var"))    {t.kind=TK_VAR;    return t;}
    if(streq(s,n,"if"))     {t.kind=TK_IF;     return t;}
    if(streq(s,n,"else"))   {t.kind=TK_ELSE;   return t;}
    if(streq(s,n,"while"))  {t.kind=TK_WHILE;  return t;}
    if(streq(s,n,"for"))    {t.kind=TK_FOR;    return t;}
    if(streq(s,n,"in"))     {t.kind=TK_IN;     return t;}
    if(streq(s,n,"return")) {t.kind=TK_RETURN; return t;}
    if(streq(s,n,"struct")) {t.kind=TK_STRUCT; return t;}
    if(streq(s,n,"match"))  {t.kind=TK_MATCH;  return t;}
    if(streq(s,n,"import")) {t.kind=TK_IMPORT; return t;}
    if(streq(s,n,"try"))    {t.kind=TK_TRY;    return t;}
    if(streq(s,n,"catch"))  {t.kind=TK_CATCH;  return t;}
    if(streq(s,n,"throw"))  {t.kind=TK_THROW;  return t;}
    if(streq(s,n,"as"))     {t.kind=TK_AS;     return t;}
    if(streq(s,n,"true"))   {t.kind=TK_TRUE;   t.ival=1; return t;}
    if(streq(s,n,"false"))  {t.kind=TK_FALSE;  t.ival=0; return t;}
    t.kind=TK_IDENT; return t;
}

Token lex_next(Lexer *l){
    const char *s=l->src; int *p=&l->pos; int end=l->len;
    Token t; t.ival=0; t.fval=0; t.line=l->line;

    /* skip whitespace (not newline) and comments */
    while(*p<end && (s[*p]==' '||s[*p]=='\t'||s[*p]=='\r')) (*p)++;
    if(*p<end && s[*p]=='-' && *p+1<end && s[*p+1]=='-'){
        while(*p<end && s[*p]!='\n') (*p)++;
    }

    if(*p>=end){t.kind=TK_EOF;t.start=s+*p;t.len=0;return t;}

    t.start=s+*p;

    /* newline */
    if(s[*p]=='\n'){t.kind=TK_NL;t.len=1;(*p)++;l->line++;t.line=l->line;return t;}

    /* number */
    if(is_digit(s[*p])){
        int64_t v=0; int start=*p;
        while(*p<end&&is_digit(s[*p])){v=v*10+(s[*p]-'0');(*p)++;}
        if(*p<end&&s[*p]=='.'&&(*p+1>=end||s[*p+1]!='.')&&(*p+1<end&&s[*p+1]>='0'&&s[*p+1]<='9')){
            (*p)++; int64_t frac=0,div=1;
            int fc=0;
            while(*p<end&&is_digit(s[*p])&&fc<3){frac=frac*10+(s[*p]-'0');div*=10;(*p)++;fc++;}
            while(*p<end&&is_digit(s[*p]))(*p)++;
            /* store as int*1000 */
            t.kind=TK_FLOAT; t.fval=v*1000 + frac*1000/div;
        } else { t.kind=TK_INT; t.ival=v; }
        t.len=*p-start; return t;
    }

    /* string */
    if(s[*p]=='"'){
        (*p)++;
        static char strbuf[512];
        int slen=0;
        while(*p<end&&s[*p]!='"'){
            if(s[*p]=='\\'&&*p+1<end){
                (*p)++;
                if(s[*p]=='n')      strbuf[slen++]='\n';
                else if(s[*p]=='t') strbuf[slen++]='\t';
                else if(s[*p]=='r') strbuf[slen++]='\r';
                else                strbuf[slen++]=s[*p];
            } else {
                strbuf[slen++]=s[*p];
            }
            (*p)++;
        }
        strbuf[slen]=0;
        t.kind=TK_STR; t.start=strbuf; t.len=slen;
        if(*p<end)(*p)++;
        return t;
    }

    /* identifier / keyword */
    if(is_alpha(s[*p])){
        int start=*p;
        while(*p<end&&is_alnum(s[*p]))(*p)++;
        t=make_kw_or_ident(s+start,*p-start);
        t.start=s+start; return t;
    }

    /* two-char operators */
    char c=s[*p]; (*p)++;
    t.len=1;
    if(c=='='&&*p<end&&s[*p]=='='){t.kind=TK_EQEQ;    t.len=2;(*p)++;return t;}
    if(c=='='&&*p<end&&s[*p]=='>'){t.kind=TK_FAT_ARROW;t.len=2;(*p)++;return t;}
    if(c=='!'&&*p<end&&s[*p]=='='){t.kind=TK_NEQ; t.len=2;(*p)++;return t;}
    if(c=='<'&&*p<end&&s[*p]=='='){t.kind=TK_LTE; t.len=2;(*p)++;return t;}
    if(c=='>'&&*p<end&&s[*p]=='='){t.kind=TK_GTE; t.len=2;(*p)++;return t;}
    if(c=='-'&&*p<end&&s[*p]=='>'){t.kind=TK_ARROW;t.len=2;(*p)++;return t;}
    if(c=='.'&&*p<end&&s[*p]=='.'){t.kind=TK_DOTDOT;t.len=2;(*p)++;return t;}
    if(c=='&'&&*p<end&&s[*p]=='&'){t.kind=TK_AND;t.len=2;(*p)++;return t;}
    if(c=='|'&&*p<end&&s[*p]=='|'){t.kind=TK_OR; t.len=2;(*p)++;return t;}

    switch(c){
    case '+': t.kind=TK_PLUS;     break;
    case '-': t.kind=TK_MINUS;    break;
    case '*': t.kind=TK_STAR;     break;
    case '/': t.kind=TK_SLASH;    break;
    case '%': t.kind=TK_PERCENT;  break;
    case '=': t.kind=TK_EQ;       break;
    case '<': t.kind=TK_LT;       break;
    case '>': t.kind=TK_GT;       break;
    case '!': t.kind=TK_BANG;     break;
    case '.': t.kind=TK_DOT;      break;
    case '@': t.kind=TK_AT;       break;
    case '(': t.kind=TK_LPAREN;   break;
    case ')': t.kind=TK_RPAREN;   break;
    case '{': t.kind=TK_LBRACE;   break;
    case '}': t.kind=TK_RBRACE;   break;
    case '[': t.kind=TK_LBRACKET; break;
    case ']': t.kind=TK_RBRACKET; break;
    case ',': t.kind=TK_COMMA;    break;
    case ':': t.kind=TK_COLON;    break;
    case ';': t.kind=TK_SEMICOLON;break;
    default:  t.kind=TK_EOF;      break;
    }
    return t;
}

void lex_init(Lexer *l, const char *src, int len){
    l->src=src; l->pos=0; l->len=len; l->line=1;
    l->cur=lex_next(l);
}

Token lex_peek(Lexer *l){ return l->cur; }

Token lex_next_tok(Lexer *l){
    Token t=l->cur;
    l->cur=lex_next(l);
    /* skip newlines between tokens */
    while(l->cur.kind==TK_NL) l->cur=lex_next(l);
    return t;
}