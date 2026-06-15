#include "yolish.h"

/* Forward declarations */
static Node *parse_expr(Lexer *l);
static Node *parse_stmt(Lexer *l);
static Node *parse_block(Lexer *l);
static Node *parse_binop(Lexer *l, int min_prec);

/* simple allocator using stack-like bump */
static Node pool[512];
static int pool_idx=0;

/* statement pointer pool — avoids static array re-entry bug */
static Node *stmt_pool[4096];
static int   stmt_pool_idx=0;
static Node *alloc_node(NodeKind k){
    if(pool_idx>=512) return pool; /* out of nodes */
    Node *n=&pool[pool_idx++];
    for(int i=0;i<(int)sizeof(Node);i++) ((char*)n)[i]=0;
    n->kind=k;
    return n;
}

static Token cur(Lexer *l){ return l->cur; }
static Token eat(Lexer *l){
    Token t=l->cur;
    l->cur=lex_next(l);
    while(l->cur.kind==TK_NL) l->cur=lex_next(l);
    return t;
}
static int check(Lexer *l, TokenKind k){ return l->cur.kind==k; }
static Token expect(Lexer *l, TokenKind k){
    if(!check(l,k)){
        char buf[64];
        int n=0; buf[n++]='['; buf[n++]='Y'; buf[n++]='S'; buf[n++]=']';
        buf[n++]=' '; buf[n++]='p'; buf[n++]='a'; buf[n++]='r'; buf[n++]='s';
        buf[n++]='e'; buf[n++]=' '; buf[n++]='e'; buf[n++]='r'; buf[n++]='r';
        buf[n++]='o'; buf[n++]='r'; buf[n++]=' '; buf[n++]='l'; buf[n++]='i';
        buf[n++]='n'; buf[n++]='e'; buf[n++]=' ';
        int64_t ln=l->line; if(ln<=0)ln=1;
        char tmp[16]; int ti=0;
        do { tmp[ti++]=(char)('0'+(ln%10)); ln/=10; } while(ln>0);
        while(ti>0) buf[n++]=tmp[--ti];
        buf[n++]='\n'; buf[n]=0;
        ys_print(buf);
    }
    return eat(l);
}
static int match_tk(Lexer *l, TokenKind k){
    if(check(l,k)){eat(l);return 1;} return 0;
}



static Node *parse_primary(Lexer *l){
    Token t=cur(l);
    if(t.kind==TK_INT){   eat(l); Node*n=alloc_node(ND_INT);n->ival=t.ival;return n;}
    if(t.kind==TK_FLOAT){ eat(l); Node*n=alloc_node(ND_FLOAT);n->fval=t.fval;return n;}
    if(t.kind==TK_TRUE||t.kind==TK_FALSE){
        eat(l); Node*n=alloc_node(ND_BOOL);n->ival=t.ival;return n;}
    if(t.kind==TK_STR){
        Node*n=alloc_node(ND_STR);
        int len=t.len<255?t.len:255;
        for(int i=0;i<len;i++) n->sval[i]=t.start[i];
        n->sval[len]=0;
        eat(l);
        return n;
    }
    if(t.kind==TK_IDENT){
        eat(l); Node*n=alloc_node(ND_IDENT);
        int len=t.len<63?t.len:63;
        for(int i=0;i<len;i++) n->name[i]=t.start[i];
        n->name[len]=0;
        /* function call */
        if(check(l,TK_LPAREN)){
            eat(l);
            Node *call=alloc_node(ND_CALL);
            for(int i=0;i<63;i++) call->name[i]=n->name[i];
            int argc=0;
            while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)&&argc<8){
                call->arg_data[argc++]=parse_expr(l);
                if(!match_tk(l,TK_COMMA)) break;
            }
            if(check(l,TK_RPAREN)) eat(l);
            call->args=call->arg_data; call->argc=argc;
            return call;
        }
        /* struct literal: Point { x: 10, y: 20 } — only uppercase names */
        if(check(l,TK_LBRACE) && n->name[0]>='A' && n->name[0]<='Z'){
            /* uppercase ident followed by { = struct literal */
            eat(l);
            Node *sl=alloc_node(ND_STRUCT_LIT);
            for(int i=0;i<63;i++) { sl->name[i]=n->name[i]; } sl->name[63]=0;
            int fc=0;
            while(!check(l,TK_RBRACE)&&!check(l,TK_EOF)&&fc<8){
                while(check(l,TK_NL)) eat(l);
                if(check(l,TK_RBRACE)) break;
                Token fn=expect(l,TK_IDENT);
                int fl=fn.len<31?fn.len:31;
                for(int i=0;i<fl;i++) sl->field_names[fc][i]=fn.start[i];
                sl->field_names[fc][fl]=0;
                if(check(l,TK_COLON)) eat(l);
                sl->arg_data[fc]=parse_expr(l);
                if(check(l,TK_COMMA)) eat(l);
                fc++;
            }
            if(check(l,TK_RBRACE)) eat(l);
            sl->args=sl->arg_data; sl->argc=fc;
            return sl;
        }
        /* index access: arr[i] */
        if(check(l,TK_LBRACKET)){
            eat(l);
            Node *idx=alloc_node(ND_INDEX);
            idx->left=n;
            idx->right=parse_expr(l);
            if(check(l,TK_RBRACKET)) eat(l);
            if(check(l,TK_EQ)){
                eat(l);
                Node *asgn=alloc_node(ND_INDEX_SET);
                asgn->left=idx; asgn->right=parse_expr(l);
                return asgn;
            }
            return idx;
        }
        /* dot access — handles chained dots: y.math.sqrt(x) */
        if(check(l,TK_DOT)){
            Node *cur2=n;
            while(check(l,TK_DOT)){
                eat(l);
                Token m=expect(l,TK_IDENT);
                Node *dot=alloc_node(ND_DOT);
                dot->left=cur2;
                int ml=m.len<63?m.len:63;
                for(int i=0;i<ml;i++) { dot->name[i]=m.start[i]; }
                dot->name[ml]=0;
                /* method call */
                if(check(l,TK_LPAREN)){
                    eat(l); dot->kind=ND_CALL;
                    /* arg_data[0] reserved for obj in method calls */
                    dot->arg_data[0]=cur2; int argc2=1;
                    while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)&&argc2<8){
                        dot->arg_data[argc2++]=parse_expr(l);
                        if(!match_tk(l,TK_COMMA)) break;
                    }
                    if(check(l,TK_RPAREN)) eat(l);
                    dot->args=dot->arg_data; dot->argc=argc2;
                    return dot;
                }
                cur2=dot;
            }
            return cur2;
        }
        return n;
    }
    if(t.kind==TK_LPAREN){
        eat(l); Node *e=parse_expr(l); expect(l,TK_RPAREN); return e;
    }
    if(t.kind==TK_MINUS){
        eat(l); Node *n=alloc_node(ND_UNOP);
        n->op=TK_MINUS; n->left=parse_primary(l); return n;
    }
    if(t.kind==TK_BANG){
        eat(l); Node *n=alloc_node(ND_UNOP);
        n->op=TK_BANG; n->left=parse_primary(l); return n;
    }
    /* Array literal: [1, 2, 3] */
    if(t.kind==TK_LBRACKET){
        eat(l);
        Node *arr=alloc_node(ND_ARRAY);
        int cnt=0;
        while(!check(l,TK_RBRACKET)&&!check(l,TK_EOF)&&cnt<16){
            arr->arg_data[cnt++]=parse_expr(l);
            if(!match_tk(l,TK_COMMA)) break;
        }
        if(check(l,TK_RBRACKET)) eat(l);
        arr->args=arr->arg_data; arr->argc=cnt;
        return arr;
    }
    /* anonymous fn literal: fn(params) { body } */
    if(t.kind==TK_FN){
        eat(l);
        Node *n=alloc_node(ND_FN_LIT);
        n->name[0]=0; /* anonymous */
        expect(l,TK_LPAREN);
        int nparams=0;
        while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)&&nparams<8){
            while(check(l,TK_NL)) eat(l);
            if(check(l,TK_RPAREN)) break;
            if(check(l,TK_IDENT)){
                Token p=eat(l);
                int pl=p.len<31?p.len:31;
                for(int i=0;i<pl;i++) { n->field_names[nparams][i]=p.start[i]; }
                n->field_names[nparams][pl]=0;
                nparams++;
                if(check(l,TK_COLON)){eat(l);eat(l);}
            } else { eat(l); }
            if(!match_tk(l,TK_COMMA)) break;
        }
        if(check(l,TK_RPAREN)) eat(l);
        if(check(l,TK_ARROW)){eat(l);eat(l);}
        n->argc=nparams;
        n->body=parse_block(l);
        return n;
    }

    /* @builtin call */
    if(t.kind==TK_AT){
        eat(l); Token nm=expect(l,TK_IDENT);
        Node *n=alloc_node(ND_CALL);
        n->name[0]='@';
        int nl=nm.len<62?nm.len:62;
        for(int i=0;i<nl;i++) n->name[i+1]=nm.start[i];
        n->name[nl+1]=0;
        expect(l,TK_LPAREN);
        static Node *args[16]; int argc=0;
        while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)){
            args[argc++]=parse_expr(l);
            if(!match_tk(l,TK_COMMA)) break;
        }
        expect(l,TK_RPAREN);
        n->args=(Node**)args; n->argc=argc; return n;
    }
    eat(l);
    return alloc_node(ND_INT); /* fallback */
}

static int prec(TokenKind k){
    switch(k){
    case TK_OR:  return 1;
    case TK_AND: return 2;
    case TK_EQEQ: case TK_NEQ: return 3;
    case TK_LT: case TK_GT: case TK_LTE: case TK_GTE: return 4;
    case TK_DOTDOT: return 4; /* range operator, same level as comparison */
    case TK_PLUS: case TK_MINUS: return 5;
    case TK_STAR: case TK_SLASH: case TK_PERCENT: return 6;
    default: return 0;
    }
}

static Node *parse_binop(Lexer *l, int min_prec){
    Node *left=parse_primary(l);
    while(1){
        int p=prec(cur(l).kind);
        if(p<=min_prec) break;
        Token op=eat(l);
        Node *right=parse_binop(l,p);
        Node *n=alloc_node(ND_BINOP);
        n->op=op.kind; n->left=left; n->right=right;
        left=n;
    }
    return left;
}

static Node *parse_expr(Lexer *l){
    Node *left=parse_binop(l,0);
    /* assignment */
    if(check(l,TK_EQ)){
        eat(l);
        Node *n=alloc_node(ND_ASSIGN);
        n->left=left; n->right=parse_expr(l);
        return n;
    }
    return left;
}


static Node *parse_block(Lexer *l){
    expect(l,TK_LBRACE);
    Node *block=alloc_node(ND_BLOCK);
    /* parse into local buffer first — nested blocks use stmt_pool during parse */
    Node *tmp[128]; int cnt=0;
    while(!check(l,TK_RBRACE)&&!check(l,TK_EOF)){
        while(check(l,TK_NL)||check(l,TK_SEMICOLON)) eat(l);
        if(check(l,TK_RBRACE)) break;
        if(cnt<128) tmp[cnt++]=parse_stmt(l);
        while(check(l,TK_NL)||check(l,TK_SEMICOLON)) eat(l);
    }
    expect(l,TK_RBRACE);
    /* now copy to permanent pool — after all nested blocks already claimed their slots */
    Node **stmts=&stmt_pool[stmt_pool_idx];
    for(int i=0;i<cnt&&stmt_pool_idx<4095;i++) stmt_pool[stmt_pool_idx++]=tmp[i];
    block->stmts=stmts; block->stmtc=cnt;
    return block;
}



static Node *parse_stmt(Lexer *l){
    Token t=cur(l);
    /* let / var */
    if(t.kind==TK_LET||t.kind==TK_VAR){
        eat(l);
        Node *n=alloc_node(t.kind==TK_LET?ND_LET:ND_VAR);
        n->is_mut=(t.kind==TK_VAR);
        Token nm=expect(l,TK_IDENT);
        int nl=nm.len<63?nm.len:63;
        for(int i=0;i<nl;i++) n->name[i]=nm.start[i];
        n->name[nl]=0;
        /* optional type */
        if(match_tk(l,TK_COLON)){
            Token ty=eat(l);
            int tl=ty.len<31?ty.len:31;
            for(int i=0;i<tl;i++) n->type[i]=ty.start[i];
            n->type[tl]=0;
        }
        if(match_tk(l,TK_EQ)) n->right=parse_expr(l);
        return n;
    }

    /* if / else if / else */
    if(t.kind==TK_IF){
        eat(l); Node *n=alloc_node(ND_IF);
        n->cond=parse_expr(l);
        n->then=parse_block(l);
        if(check(l,TK_ELSE)){
            eat(l);
            if(check(l,TK_IF)) n->els=parse_stmt(l); /* else if → nested ND_IF */
            else                n->els=parse_block(l);
        }
        return n;
    }

    /* while */
    if(t.kind==TK_WHILE){
        eat(l); Node *n=alloc_node(ND_WHILE);
        n->cond=parse_expr(l); n->body=parse_block(l); return n;
    }

    /* for item in expr { } */
    if(t.kind==TK_FOR){
        eat(l); Node *n=alloc_node(ND_FOR);
        Token var=expect(l,TK_IDENT);
        int vl=var.len<63?var.len:63;
        for(int i=0;i<vl;i++) { n->name[i]=var.start[i]; } n->name[vl]=0;
        expect(l,TK_IN);
        n->cond=parse_expr(l); /* iterable: array or a..b range binop */
        n->body=parse_block(l);
        return n;
    }

    /* return */
    if(t.kind==TK_RETURN){
        eat(l); Node *n=alloc_node(ND_RETURN);
        if(!check(l,TK_NL)&&!check(l,TK_SEMICOLON)&&!check(l,TK_RBRACE))
            n->right=parse_expr(l);
        return n;
    }

    /* struct Point { x, y } */
    if(t.kind==TK_STRUCT){
        eat(l);
        Node *n=alloc_node(ND_STRUCT);
        Token nm=expect(l,TK_IDENT);
        int nl=nm.len<63?nm.len:63;
        for(int i=0;i<nl;i++) { n->name[i]=nm.start[i]; } n->name[nl]=0;
        expect(l,TK_LBRACE);
        static Node *fields[16]; int fc=0;
        while(!check(l,TK_RBRACE)&&!check(l,TK_EOF)){
            while(check(l,TK_NL)||check(l,TK_COMMA)) eat(l);
            if(check(l,TK_RBRACE)) break;
            Token fn=expect(l,TK_IDENT);
            Node *fld=alloc_node(ND_IDENT);
            int fl=fn.len<63?fn.len:63;
            for(int i=0;i<fl;i++) { fld->name[i]=fn.start[i]; } fld->name[fl]=0;
            if(check(l,TK_COMMA)) eat(l);
            fields[fc++]=fld;
        }
        if(check(l,TK_RBRACE)) eat(l);
        n->stmts=(Node**)fields; n->stmtc=fc;
        return n;
    }
    /* @intent("arg") / @audit("arg") annotation before fn */
    if(t.kind==TK_AT){
        eat(l);
        Token ann=expect(l,TK_IDENT);
        /* annotation name into a temp buffer */
        char ann_name[32]; int al=ann.len<31?ann.len:31;
        for(int i=0;i<al;i++) { ann_name[i]=ann.start[i]; } ann_name[al]=0;
        /* parse optional ("arg") */
        char ann_arg[64]; ann_arg[0]=0;
        if(check(l,TK_LPAREN)){
            eat(l);
            if(check(l,TK_STR)){
                Token av=eat(l);
                int vl=av.len<63?av.len:63;
                for(int i=0;i<vl;i++) { ann_arg[i]=av.start[i]; } ann_arg[vl]=0;
            }
            if(check(l,TK_RPAREN)) eat(l);
        }
        while(check(l,TK_NL)) eat(l);
        /* next statement must be fn */
        if(!check(l,TK_FN)){
            ys_print("[YS] annotation must precede fn\n");
            return alloc_node(ND_INT);
        }
        eat(l); /* eat TK_FN */
        Node *n=alloc_node(ND_FN);
        /* copy annotation into node */
        for(int i=0;ann_name[i]&&i<31;i++) n->type[i]=ann_name[i];
        n->type[31]=0;
        for(int i=0;ann_arg[i]&&i<255;i++) n->sval[i]=ann_arg[i];
        n->sval[255]=0;
        /* parse rest of fn (name, params, body) */
        Token nm=expect(l,TK_IDENT);
        int nl2=nm.len<63?nm.len:63;
        for(int i=0;i<nl2;i++) { n->name[i]=nm.start[i]; } n->name[nl2]=0;
        expect(l,TK_LPAREN);
        int nparams=0;
        while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)&&nparams<8){
            while(check(l,TK_NL)) eat(l);
            if(check(l,TK_RPAREN)) break;
            if(check(l,TK_IDENT)){
                Token p=eat(l);
                int pl=p.len<31?p.len:31;
                for(int i=0;i<pl;i++) n->field_names[nparams][i]=p.start[i];
                n->field_names[nparams][pl]=0;
                nparams++;
                if(check(l,TK_COLON)){eat(l);eat(l);}
            } else { eat(l); }
            if(!match_tk(l,TK_COMMA)) break;
        }
        n->argc=nparams;
        expect(l,TK_RPAREN);
        if(check(l,TK_ARROW)){eat(l);eat(l);}
        n->body=parse_block(l);
        return n;
    }

    /* fn */
    if(t.kind==TK_FN){
        eat(l); Node *n=alloc_node(ND_FN);
        Token nm=expect(l,TK_IDENT);
        int nl=nm.len<63?nm.len:63;
        for(int i=0;i<nl;i++) n->name[i]=nm.start[i];
        n->name[nl]=0;
        expect(l,TK_LPAREN);
        /* parse parameter names into field_names[] */
        int nparams=0;
        while(!check(l,TK_RPAREN)&&!check(l,TK_EOF)&&nparams<8){
            while(check(l,TK_NL)) eat(l);
            if(check(l,TK_RPAREN)) break;
            if(check(l,TK_IDENT)){
                Token p=eat(l);
                int pl=p.len<31?p.len:31;
                for(int i=0;i<pl;i++) n->field_names[nparams][i]=p.start[i];
                n->field_names[nparams][pl]=0;
                nparams++;
                /* skip optional type annotation: a: str */
                if(check(l,TK_COLON)){eat(l);eat(l);}
            } else {
                eat(l);
            }
            if(!match_tk(l,TK_COMMA)) break;
        }
        n->argc=nparams;
        expect(l,TK_RPAREN);
        if(check(l,TK_ARROW)){eat(l);eat(l);} /* skip return type */
        n->body=parse_block(l);
        return n;
    }
    
    /* match x { pat => body, ... } */
    if(t.kind==TK_MATCH){
        eat(l);
        Node *n=alloc_node(ND_MATCH);
        n->cond=parse_expr(l);
        expect(l,TK_LBRACE);
        int arms=0;
        while(!check(l,TK_RBRACE)&&!check(l,TK_EOF)&&arms<8){
            while(check(l,TK_NL)||check(l,TK_SEMICOLON)) eat(l);
            if(check(l,TK_RBRACE)) break;
            /* parse pattern: int, float, str, bool, _, or range a..b */
            Node *pat=NULL;
            if(check(l,TK_IDENT)&&l->cur.len==1&&l->cur.start[0]=='_'){
                eat(l); pat=alloc_node(ND_IDENT);
                pat->name[0]='_'; pat->name[1]=0;
            } else {
                pat=parse_binop(l,0); /* parse_binop avoids assignment trap */
            }
            /* expect => (TK_FAT_ARROW) */
            if(check(l,TK_FAT_ARROW)) eat(l);
            /* body: block or single expr */
            Node *body=NULL;
            if(check(l,TK_LBRACE)) body=parse_block(l);
            else body=parse_expr(l);
            n->arg_data[arms*2  ]=pat;
            n->arg_data[arms*2+1]=body;
            arms++;
            while(check(l,TK_NL)||check(l,TK_SEMICOLON)||check(l,TK_COMMA)) eat(l);
        }
        expect(l,TK_RBRACE);
        n->args=n->arg_data;
        n->argc=arms;
        return n;
    }

    /* throw expr */
    if(t.kind==TK_THROW){
        eat(l);
        Node *n=alloc_node(ND_THROW);
        n->right=parse_expr(l);
        return n;
    }

    /* try { } catch(e) { } */
    if(t.kind==TK_TRY){
        eat(l);
        Node *n=alloc_node(ND_TRY);
        n->then=parse_block(l);   /* try body */
        n->name[0]=0;
        if(check(l,TK_CATCH)){
            eat(l);
            /* optional (varname) */
            if(check(l,TK_LPAREN)){
                eat(l);
                if(check(l,TK_IDENT)){
                    Token vt=eat(l);
                    int vl=vt.len<63?vt.len:63;
                    for(int i=0;i<vl;i++) { n->name[i]=vt.start[i]; }
                    n->name[vl]=0;
                }
                if(check(l,TK_RPAREN)) eat(l);
            }
            n->els=parse_block(l); /* catch body */
        }
        return n;
    }

    /* import "file.y" [as name] */
    if(t.kind==TK_IMPORT){
        eat(l);
        Node *n=alloc_node(ND_IMPORT);
        if(check(l,TK_STR)){
            Token pt=eat(l);
            int pl=pt.len<255?pt.len:255;
            for(int i=0;i<pl;i++) n->sval[i]=pt.start[i];
            n->sval[pl]=0;
        }
        /* optional: as namespace_name → ND_MODULE */
        if(check(l,TK_AS)){
            eat(l);
            Token nm=expect(l,TK_IDENT);
            n->kind=ND_MODULE;
            int nl=nm.len<63?nm.len:63;
            for(int i=0;i<nl;i++) n->name[i]=nm.start[i];
            n->name[nl]=0;
        }
        return n;
    }

    /* pragma / comment skip */
    if(t.kind==TK_IDENT&&t.start[0]=='#'){
        while(!check(l,TK_NL)&&!check(l,TK_EOF)) eat(l);
        return alloc_node(ND_INT);
    }
    return parse_expr(l);
}

Node *parse_program(Lexer *l){
    Node *prog=alloc_node(ND_BLOCK);
    Node *tmp[256]; int cnt=0;
    while(!check(l,TK_EOF)){
        while(check(l,TK_NL)||check(l,TK_SEMICOLON)) eat(l);
        if(check(l,TK_EOF)) break;
        if(cnt<256) tmp[cnt++]=parse_stmt(l);
        while(check(l,TK_NL)||check(l,TK_SEMICOLON)) eat(l);
    }
    Node **stmts=&stmt_pool[stmt_pool_idx];
    for(int i=0;i<cnt&&stmt_pool_idx<4095;i++) stmt_pool[stmt_pool_idx++]=tmp[i];
    prog->stmts=stmts; prog->stmtc=cnt;
    return prog;
}
/* Pool checkpoint for sub-parses (e.g. string interpolation) */
static int pool_save_idx=0;
static int stmt_pool_save_idx=0;
void parser_pool_save(void){
    pool_save_idx=pool_idx;
    stmt_pool_save_idx=stmt_pool_idx;
}
void parser_pool_restore(void){
    pool_idx=pool_save_idx;
    stmt_pool_idx=stmt_pool_save_idx;
}