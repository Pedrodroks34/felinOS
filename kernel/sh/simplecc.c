/*
 * simplecc - small C compiler that runs inside the FelinOS shell.
 *
 * It reads a program from the keyboard (end with Ctrl+D), compiles it to
 * native x86-64 machine code and runs main() in kernel mode.
 *
 * Language: int/void functions (up to 6 int parameters), int locals,
 * if/else, while, for, return, + - * / %, comparisons, && || !, unary -,
 * char and string literals, // and block comments, and the builtins
 * putchar(c), puts(s), getchar().
 *
 * Old versions passed a fake pointer to vfs_read() (kernel panic, page fault
 * at 0x19), emitted 32-bit code with int 0x80 that cannot run in the kernel,
 * corrupted stack offsets (8-bit displacements, no frame size patching) and
 * leaked every identifier.  Generated code now only calls kernel helpers,
 * and runaway loops / recursion are aborted by cc_tick().
 */
#ifdef CC_HOST_TEST
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#define kprintf printf
static void console_putchar(char c) { putchar(c); }
static int input_getkey(void) { int c = getchar(); return c == EOF ? 4 : c; }
#else
#include <stdint.h>
#include <stddef.h>
#include "lib/string.h"
#include "lib/format.h"
#include "console.h"
#include "drivers/input.h"
#endif

#define MAX_TOKENS   2048
#define MAX_SYMS     128
#define MAX_FUNCS    64
#define MAX_FIXUPS   256
#define MAX_CODE     16384
#define MAX_SRC      8192
#define MAX_STR      2048
#define MAX_ARGS     6
#define MAX_STEPS    50000000u   /* loop iterations + calls before abort */
#define MAX_STACK    16384       /* bytes of stack generated code may use */

typedef enum {
    TK_EOF, TK_IDENT, TK_NUMBER, TK_STRING,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE, TK_AND, TK_OR, TK_NOT,
    TK_ASSIGN, TK_SEMI, TK_COMMA, TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_RETURN, TK_INT, TK_VOID,
} TokenType;

struct Token { TokenType type; int val; int line; char name[32]; };
struct Sym   { char name[32]; int offset; };
struct Func  { char name[32]; int addr; int nargs; };
struct Fixup { int pos; char name[32]; int line; int nargs; };

static const char *src;
static int src_pos, line_no;
static char srcbuf[MAX_SRC];
static struct Token tokens[MAX_TOKENS];
static int token_count, token_pos;
static struct Sym syms[MAX_SYMS];
static int sym_count, frame_size;
static struct Func funcs[MAX_FUNCS];
static int func_count;
static struct Fixup fixups[MAX_FIXUPS];
static int fixup_count;
static uint8_t code[MAX_CODE] __attribute__((aligned(4096)));
static int code_size;
static char strpool[MAX_STR];
static int str_size;
static int had_error;
static void *parse_jb[5];
static void *run_jb[5];
static uintptr_t stack_base;
static uint32_t steps;
static int abort_reason;

/* ---------------------------------------------------------------- errors */

static void fail(const char *msg) {
    if (!had_error) {
        had_error = 1;
        kprintf("Error (line %d): %s\n", token_pos < token_count ? tokens[token_pos].line : line_no, msg);
    }
    __builtin_longjmp(parse_jb, 1);
}

/* --------------------------------------------------------------- runtime */

static void rt_putchar(int c) { console_putchar((char)c); }
static void rt_puts(const char *s) { while (*s) console_putchar(*s++); console_putchar('\n'); }
static int rt_getchar(void) {
    int k = input_getkey();
    if (k == 4 || k == 3) return -1;
    if (k == '\r') k = '\n';
    if (k < 0 || k > 0xFF) return -1;
    console_putchar((char)k);
    return k;
}
static void rt_tick(void) {
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (stack_base - sp > MAX_STACK) { abort_reason = 2; __builtin_longjmp(run_jb, 1); }
    if (++steps > MAX_STEPS)         { abort_reason = 1; __builtin_longjmp(run_jb, 1); }
}
static void rt_divzero(void) { abort_reason = 3; __builtin_longjmp(run_jb, 1); }

/* --------------------------------------------------------------- emitters */

static void eb(int b) {
    if (code_size >= MAX_CODE) fail("program too large");
    code[code_size++] = (uint8_t)b;
}
static void e32(uint32_t v) { eb(v); eb(v >> 8); eb(v >> 16); eb(v >> 24); }
static void e64(uint64_t v) { e32((uint32_t)v); e32((uint32_t)(v >> 32)); }
static void patch32(int pos, uint32_t v) {
    code[pos] = v; code[pos + 1] = v >> 8; code[pos + 2] = v >> 16; code[pos + 3] = v >> 24;
}
static void call_abs(const void *fn) {
    eb(0x48); eb(0xB8); e64((uint64_t)(uintptr_t)fn);   /* mov rax, imm64 */
    eb(0xFF); eb(0xD0);                                 /* call rax       */
}
static int jump(int op2) {              /* jmp / jcc rel32, returns patch pos */
    if (op2) { eb(0x0F); eb(op2); } else eb(0xE9);
    e32(0);
    return code_size - 4;
}
static void patch_here(int pos) { patch32(pos, (uint32_t)(code_size - (pos + 4))); }
static void jump_to(int target) { eb(0xE9); e32((uint32_t)(target - (code_size + 4))); }

/* --------------------------------------------------------------- lexer */

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static void add_token(TokenType t, int val, const char *name) {
    if (token_count >= MAX_TOKENS) { kprintf("Error: too many tokens\n"); had_error = 1; return; }
    struct Token *k = &tokens[token_count++];
    k->type = t; k->val = val; k->line = line_no; k->name[0] = 0;
    if (name) { strncpy(k->name, name, 31); k->name[31] = 0; }
}

static int esc(char c) {
    switch (c) { case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
                 case '0': return 0; default: return c; }
}

static void lex(void) {
    token_count = 0; line_no = 1; src_pos = 0;
    while (!had_error) {
        char c = src[src_pos];
        if (!c) break;
        if (c == '\n') { line_no++; src_pos++; continue; }
        if ((unsigned char)c <= ' ') { src_pos++; continue; }
        if (c == '/' && src[src_pos + 1] == '/') { while (src[src_pos] && src[src_pos] != '\n') src_pos++; continue; }
        if (c == '/' && src[src_pos + 1] == '*') {
            src_pos += 2;
            while (src[src_pos] && !(src[src_pos] == '*' && src[src_pos + 1] == '/')) {
                if (src[src_pos] == '\n') line_no++;
                src_pos++;
            }
            if (src[src_pos]) src_pos += 2;
            continue;
        }
        if (is_alpha(c)) {
            char id[32]; int n = 0;
            while (is_alpha(src[src_pos]) || is_digit(src[src_pos])) { if (n < 31) id[n++] = src[src_pos]; src_pos++; }
            id[n] = 0;
            TokenType t = TK_IDENT;
            if (!strcmp(id, "if")) t = TK_IF; else if (!strcmp(id, "else")) t = TK_ELSE;
            else if (!strcmp(id, "while")) t = TK_WHILE; else if (!strcmp(id, "for")) t = TK_FOR;
            else if (!strcmp(id, "return")) t = TK_RETURN; else if (!strcmp(id, "int")) t = TK_INT;
            else if (!strcmp(id, "void")) t = TK_VOID;
            add_token(t, 0, id);
        } else if (is_digit(c)) {
            int v = 0;
            while (is_digit(src[src_pos])) v = v * 10 + (src[src_pos++] - '0');
            add_token(TK_NUMBER, v, 0);
        } else if (c == '\'') {
            int v = src[src_pos + 1]; src_pos += 2;
            if (v == '\\') { v = esc(src[src_pos]); src_pos++; }
            if (src[src_pos] != '\'') { kprintf("Error (line %d): bad char literal\n", line_no); had_error = 1; break; }
            src_pos++;
            add_token(TK_NUMBER, v, 0);
        } else if (c == '"') {
            int start = str_size;
            src_pos++;
            while (src[src_pos] && src[src_pos] != '"') {
                int ch = src[src_pos++];
                if (ch == '\\') ch = esc(src[src_pos++]);
                if (str_size >= MAX_STR - 1) { kprintf("Error (line %d): string pool full\n", line_no); had_error = 1; break; }
                strpool[str_size++] = (char)ch;
            }
            if (had_error) break;
            if (src[src_pos] != '"') { kprintf("Error (line %d): unterminated string\n", line_no); had_error = 1; break; }
            src_pos++;
            strpool[str_size++] = 0;
            add_token(TK_STRING, start, 0);
        } else {
            char n = src[src_pos + 1];
            src_pos++;
#define TWO(ch, t2, t1) if (n == ch) { src_pos++; add_token(t2, 0, 0); } else add_token(t1, 0, 0); break
            switch (c) {
                case '+': add_token(TK_PLUS, 0, 0); break;
                case '-': add_token(TK_MINUS, 0, 0); break;
                case '*': add_token(TK_STAR, 0, 0); break;
                case '/': add_token(TK_SLASH, 0, 0); break;
                case '%': add_token(TK_PERCENT, 0, 0); break;
                case '=': TWO('=', TK_EQ, TK_ASSIGN);
                case '!': TWO('=', TK_NEQ, TK_NOT);
                case '<': TWO('=', TK_LE, TK_LT);
                case '>': TWO('=', TK_GE, TK_GT);
                case '&': if (n == '&') { src_pos++; add_token(TK_AND, 0, 0); break; }
                          kprintf("Error (line %d): unexpected '&'\n", line_no); had_error = 1; break;
                case '|': if (n == '|') { src_pos++; add_token(TK_OR, 0, 0); break; }
                          kprintf("Error (line %d): unexpected '|'\n", line_no); had_error = 1; break;
                case ';': add_token(TK_SEMI, 0, 0); break;
                case ',': add_token(TK_COMMA, 0, 0); break;
                case '(': add_token(TK_LPAREN, 0, 0); break;
                case ')': add_token(TK_RPAREN, 0, 0); break;
                case '{': add_token(TK_LBRACE, 0, 0); break;
                case '}': add_token(TK_RBRACE, 0, 0); break;
                default:
                    kprintf("Error (line %d): unexpected character '%c'\n", line_no, c);
                    had_error = 1; break;
            }
#undef TWO
        }
    }
    add_token(TK_EOF, 0, 0);
}

/* --------------------------------------------------------------- parser */

static struct Token *cur(void) { return &tokens[token_pos]; }
static int accept(TokenType t) { if (cur()->type == t) { token_pos++; return 1; } return 0; }
static void expect(TokenType t, const char *msg) { if (!accept(t)) fail(msg); }

static int find_sym(const char *name) {
    for (int i = sym_count - 1; i >= 0; i--) if (!strcmp(syms[i].name, name)) return i;
    return -1;
}
static int add_sym(const char *name) {
    if (sym_count >= MAX_SYMS) fail("too many variables");
    if (frame_size > 0x7000) fail("stack frame too large");
    frame_size += 4;
    strcpy(syms[sym_count].name, name);
    syms[sym_count].offset = -frame_size;
    return sym_count++;
}
static int find_func(const char *name) {
    for (int i = 0; i < func_count; i++) if (!strcmp(funcs[i].name, name)) return i;
    return -1;
}

static void load_local(int off)  { eb(0x8B); eb(0x85); e32((uint32_t)off); }   /* mov eax,[rbp+off] */
static void store_local(int off) { eb(0x89); eb(0x85); e32((uint32_t)off); }   /* mov [rbp+off],eax */

static void gen_expr(void);

static void gen_call(const char *name) {
    int nargs = 0;
    expect(TK_LPAREN, "expected '('");
    if (cur()->type != TK_RPAREN) {
        do {
            if (nargs >= MAX_ARGS) fail("too many arguments");
            gen_expr(); eb(0x50); nargs++;                       /* push rax */
        } while (accept(TK_COMMA));
    }
    expect(TK_RPAREN, "expected ')'");

    const void *builtin = 0; int bargs = 0;
    if (!strcmp(name, "putchar")) { builtin = rt_putchar; bargs = 1; }
    else if (!strcmp(name, "puts")) { builtin = rt_puts; bargs = 1; }
    else if (!strcmp(name, "getchar")) { builtin = rt_getchar; bargs = 0; }
    if (builtin && nargs != bargs) fail("wrong number of arguments to builtin");

    for (int i = nargs - 1; i >= 0; i--) {
        switch (i) {
            case 0: eb(0x5F); break;                    /* pop rdi */
            case 1: eb(0x5E); break;                    /* pop rsi */
            case 2: eb(0x5A); break;                    /* pop rdx */
            case 3: eb(0x59); break;                    /* pop rcx */
            case 4: eb(0x41); eb(0x58); break;          /* pop r8  */
            default: eb(0x41); eb(0x59); break;         /* pop r9  */
        }
    }
    if (builtin) { call_abs(builtin); return; }

    int f = find_func(name);
    if (f >= 0) {
        if (funcs[f].nargs != nargs) fail("wrong number of arguments");
        eb(0xE8); e32((uint32_t)(funcs[f].addr - (code_size + 4)));
    } else {
        if (fixup_count >= MAX_FIXUPS) fail("too many calls");
        eb(0xE8); e32(0);
        fixups[fixup_count].pos = code_size - 4;
        fixups[fixup_count].line = cur()->line;
        fixups[fixup_count].nargs = nargs;
        strcpy(fixups[fixup_count].name, name);
        fixup_count++;
    }
}

static void gen_primary(void) {
    struct Token *t = cur();
    if (t->type == TK_NUMBER) {
        token_pos++; eb(0xB8); e32((uint32_t)t->val);
    } else if (t->type == TK_STRING) {
        token_pos++; eb(0xB8); e32((uint32_t)(uintptr_t)&strpool[t->val]);   /* kernel is identity mapped < 4 GiB */
    } else if (t->type == TK_IDENT) {
        char name[32]; strcpy(name, t->name);
        token_pos++;
        if (cur()->type == TK_LPAREN) gen_call(name);
        else {
            int i = find_sym(name);
            if (i < 0) { kprintf("Error (line %d): undeclared variable '%s'\n", t->line, name); had_error = 1; __builtin_longjmp(parse_jb, 1); }
            load_local(syms[i].offset);
        }
    } else if (t->type == TK_LPAREN) {
        token_pos++; gen_expr(); expect(TK_RPAREN, "expected ')'");
    } else fail("expected expression");
}

static void gen_unary(void) {
    if (accept(TK_MINUS)) { gen_unary(); eb(0xF7); eb(0xD8); }                      /* neg eax */
    else if (accept(TK_NOT)) { gen_unary(); eb(0x85); eb(0xC0); eb(0x0F); eb(0x94); eb(0xC0); eb(0x0F); eb(0xB6); eb(0xC0); }
    else gen_primary();
}

static void binop_begin(void) { eb(0x50); }                     /* push rax (lhs) */

/* rhs in eax, lhs on stack: leave lhs in eax, rhs in ecx */
static void binop_regs(void) { eb(0x89); eb(0xC1); eb(0x58); }  /* mov ecx,eax ; pop rax */
static void cmp_set(int cc) {
    eb(0x39); eb(0xC8);                                         /* cmp eax,ecx */
    eb(0x0F); eb(cc); eb(0xC0);                                 /* setcc al    */
    eb(0x0F); eb(0xB6); eb(0xC0);                               /* movzx eax,al*/
}

static void gen_mul(void) {
    gen_unary();
    for (;;) {
        TokenType op = cur()->type;
        if (op != TK_STAR && op != TK_SLASH && op != TK_PERCENT) break;
        token_pos++; binop_begin(); gen_unary(); binop_regs();
        if (op == TK_STAR) { eb(0x0F); eb(0xAF); eb(0xC1); }
        else {
            eb(0x85); eb(0xC9);                                 /* test ecx,ecx */
            int ok = jump(0x85);                                /* jnz ok       */
            call_abs(rt_divzero);
            patch_here(ok);
            eb(0x99);                                           /* cdq          */
            eb(0xF7); eb(0xF9);                                 /* idiv ecx     */
            if (op == TK_PERCENT) { eb(0x89); eb(0xD0); }       /* mov eax,edx  */
        }
    }
}
static void gen_add(void) {
    gen_mul();
    for (;;) {
        TokenType op = cur()->type;
        if (op != TK_PLUS && op != TK_MINUS) break;
        token_pos++; binop_begin(); gen_mul(); binop_regs();
        if (op == TK_PLUS) { eb(0x01); eb(0xC8); } else { eb(0x29); eb(0xC8); }
    }
}
static void gen_rel(void) {
    gen_add();
    for (;;) {
        TokenType op = cur()->type;
        int cc;
        switch (op) { case TK_LT: cc = 0x9C; break; case TK_LE: cc = 0x9E; break;
                      case TK_GT: cc = 0x9F; break; case TK_GE: cc = 0x9D; break; default: return; }
        token_pos++; binop_begin(); gen_add(); binop_regs(); cmp_set(cc);
    }
}
static void gen_eq(void) {
    gen_rel();
    for (;;) {
        TokenType op = cur()->type;
        if (op != TK_EQ && op != TK_NEQ) break;
        token_pos++; binop_begin(); gen_rel(); binop_regs(); cmp_set(op == TK_EQ ? 0x94 : 0x95);
    }
}
static void to_bool(void) { eb(0x85); eb(0xC0); eb(0x0F); eb(0x95); eb(0xC0); eb(0x0F); eb(0xB6); eb(0xC0); }
static void gen_and(void) {
    gen_eq();
    if (cur()->type != TK_AND) return;
    int patches[16]; int n = 0;
    while (accept(TK_AND)) {
        if (n >= 16) fail("expression too complex");
        eb(0x85); eb(0xC0); patches[n++] = jump(0x84);          /* test; jz false */
        gen_eq();
    }
    to_bool();
    int done = jump(0);
    for (int i = 0; i < n; i++) patch_here(patches[i]);
    eb(0x31); eb(0xC0);                                         /* xor eax,eax */
    patch_here(done);
}
static void gen_expr(void) {                                    /* || level */
    gen_and();
    if (cur()->type != TK_OR) return;
    int patches[16]; int n = 0;
    while (accept(TK_OR)) {
        if (n >= 16) fail("expression too complex");
        eb(0x85); eb(0xC0); patches[n++] = jump(0x85);          /* test; jnz true */
        gen_and();
    }
    to_bool();
    int done = jump(0);
    for (int i = 0; i < n; i++) patch_here(patches[i]);
    eb(0xB8); e32(1);
    patch_here(done);
}

static void gen_stmt(void);

static void gen_assign_or_call(void) {
    /* IDENT '=' expr | IDENT '(' args ')' ; caller consumes ';' */
    struct Token *t = cur();
    if (t->type != TK_IDENT) fail("expected statement");
    char name[32]; strcpy(name, t->name);
    int line = t->line;
    token_pos++;
    if (cur()->type == TK_LPAREN) { gen_call(name); return; }
    if (!accept(TK_ASSIGN)) fail("expected '=' or '('");
    int i = find_sym(name);
    if (i < 0) { kprintf("Error (line %d): undeclared variable '%s'\n", line, name); had_error = 1; __builtin_longjmp(parse_jb, 1); }
    gen_expr();
    store_local(syms[i].offset);
}

static void gen_decl(void) {
    do {
        if (cur()->type != TK_IDENT) fail("expected variable name");
        char name[32]; strcpy(name, cur()->name);
        token_pos++;
        int i = add_sym(name);
        if (accept(TK_ASSIGN)) { gen_expr(); store_local(syms[i].offset); }
        else { eb(0xC7); eb(0x85); e32((uint32_t)syms[i].offset); e32(0); }   /* mov dword [rbp+off],0 */
    } while (accept(TK_COMMA));
    expect(TK_SEMI, "expected ';'");
}

static void gen_cond_jz(int *patch) {
    expect(TK_LPAREN, "expected '('");
    gen_expr();
    expect(TK_RPAREN, "expected ')'");
    eb(0x85); eb(0xC0);
    *patch = jump(0x84);
}

static void gen_stmt(void) {
    TokenType t = cur()->type;
    int scope = sym_count;
    if (t == TK_IF) {
        token_pos++;
        int jz; gen_cond_jz(&jz);
        gen_stmt();
        if (accept(TK_ELSE)) {
            int jmp = jump(0);
            patch_here(jz);
            gen_stmt();
            patch_here(jmp);
        } else patch_here(jz);
    } else if (t == TK_WHILE) {
        token_pos++;
        int start = code_size, jz;
        gen_cond_jz(&jz);
        gen_stmt();
        call_abs(rt_tick);
        jump_to(start);
        patch_here(jz);
    } else if (t == TK_FOR) {
        token_pos++;
        expect(TK_LPAREN, "expected '('");
        if (cur()->type == TK_INT) { token_pos++; gen_decl(); }
        else { if (cur()->type != TK_SEMI) gen_assign_or_call(); expect(TK_SEMI, "expected ';'"); }
        int cond = code_size, jz = -1;
        if (cur()->type != TK_SEMI) { gen_expr(); eb(0x85); eb(0xC0); jz = jump(0x84); }
        expect(TK_SEMI, "expected ';'");
        int to_body = jump(0);
        int step = code_size;
        if (cur()->type != TK_RPAREN) gen_assign_or_call();
        expect(TK_RPAREN, "expected ')'");
        call_abs(rt_tick);
        jump_to(cond);
        patch_here(to_body);
        gen_stmt();
        jump_to(step);
        if (jz >= 0) patch_here(jz);
    } else if (t == TK_RETURN) {
        token_pos++;
        if (cur()->type != TK_SEMI) gen_expr(); else { eb(0x31); eb(0xC0); }
        expect(TK_SEMI, "expected ';'");
        eb(0xC9); eb(0xC3);                                     /* leave; ret */
    } else if (t == TK_LBRACE) {
        token_pos++;
        while (cur()->type != TK_RBRACE) {
            if (cur()->type == TK_EOF) fail("missing '}'");
            gen_stmt();
        }
        token_pos++;
    } else if (t == TK_INT) {
        token_pos++; gen_decl();
    } else if (t == TK_SEMI) {
        token_pos++;
    } else {
        gen_assign_or_call();
        expect(TK_SEMI, "expected ';'");
    }
    /* Declarations stay visible until the end of the enclosing block; only
     * blocks and for-statements (which own their declarations) restore it. */
    if (t == TK_LBRACE || t == TK_FOR) sym_count = scope;
}

static void gen_function(void) {
    token_pos++;                                                /* int / void */
    if (cur()->type != TK_IDENT) fail("expected function name");
    if (func_count >= MAX_FUNCS) fail("too many functions");
    struct Func *f = &funcs[func_count];
    strcpy(f->name, cur()->name);
    if (find_func(f->name) >= 0) fail("function defined twice");
    token_pos++;
    f->addr = code_size; f->nargs = 0;
    func_count++;                                               /* visible to itself: recursion */
    expect(TK_LPAREN, "expected '('");
    sym_count = 0; frame_size = 0;

    /* prologue: push rbp; mov rbp,rsp; sub rsp,imm32 (patched later) */
    eb(0x55); eb(0x48); eb(0x89); eb(0xE5);
    eb(0x48); eb(0x81); eb(0xEC); e32(0);
    int frame_patch = code_size - 4;

    int nparams = 0;
    if (cur()->type == TK_VOID && tokens[token_pos + 1].type == TK_RPAREN) token_pos++;
    else if (cur()->type != TK_RPAREN) {
        do {
            expect(TK_INT, "expected 'int' parameter");
            if (cur()->type != TK_IDENT) fail("expected parameter name");
            if (nparams >= MAX_ARGS) fail("too many parameters");
            int i = add_sym(cur()->name);
            token_pos++;
            /* store incoming register argument in its slot */
            static const uint8_t st[MAX_ARGS][3] = {
                {0x89, 0xBD, 0}, {0x89, 0xB5, 0}, {0x89, 0x95, 0},
                {0x89, 0x8D, 0}, {0x44, 0x89, 0x85}, {0x44, 0x89, 0x8D} };
            if (nparams < 4) { eb(st[nparams][0]); eb(st[nparams][1]); }
            else { eb(st[nparams][0]); eb(st[nparams][1]); eb(st[nparams][2]); }
            e32((uint32_t)syms[i].offset);
            nparams++;
        } while (accept(TK_COMMA));
    }
    f->nargs = nparams;
    expect(TK_RPAREN, "expected ')'");
    call_abs(rt_tick);          /* after the argument registers were saved */
    if (cur()->type != TK_LBRACE) fail("expected '{'");
    token_pos++;
    while (cur()->type != TK_RBRACE) {
        if (cur()->type == TK_EOF) fail("missing '}'");
        gen_stmt();
    }
    token_pos++;
    eb(0x31); eb(0xC0); eb(0xC9); eb(0xC3);                     /* implicit return 0 */
    patch32(frame_patch, (uint32_t)((frame_size + 15) & ~15));
    sym_count = 0;
}

/* ------------------------------------------------------------------ main */

static int read_source(void) {
    int pos = 0;
    kprintf("Enter code (end with Ctrl+D):\n");
    for (;;) {
        int k = input_getkey();
        if (k == 4) break;
        if (k == 3) { kprintf("^C\n"); return -1; }
        if (k == '\r') k = '\n';
        if (k == '\b' || k == 127) {
            if (pos > 0) { pos--; console_putchar('\b'); }
            continue;
        }
        if (k < 0 || k > 0xFF || (k < 32 && k != '\n' && k != '\t')) continue;
        if (pos >= MAX_SRC - 1) continue;
        srcbuf[pos++] = (char)k;
        console_putchar((char)k);
    }
    srcbuf[pos] = 0;
    console_putchar('\n');
    return pos;
}

static int compile(void) {
    had_error = 0; token_pos = 0; token_count = 0; sym_count = 0; frame_size = 0;
    func_count = 0; fixup_count = 0; code_size = 0; str_size = 0;
    src = srcbuf;
    if (__builtin_setjmp(parse_jb)) return -1;
    lex();
    if (had_error) return -1;
    while (cur()->type != TK_EOF) {
        if (cur()->type != TK_INT && cur()->type != TK_VOID) fail("expected function definition");
        gen_function();
    }
    for (int i = 0; i < fixup_count; i++) {
        int f = find_func(fixups[i].name);
        if (f < 0) {
            kprintf("Error (line %d): undefined function '%s'\n", fixups[i].line, fixups[i].name);
            return -1;
        }
        if (funcs[f].nargs != fixups[i].nargs) {
            kprintf("Error (line %d): wrong number of arguments to '%s'\n", fixups[i].line, fixups[i].name);
            return -1;
        }
        patch32(fixups[i].pos, (uint32_t)(funcs[f].addr - (fixups[i].pos + 4)));
    }
    if (find_func("main") < 0) { kprintf("Error: no main() function\n"); return -1; }
    return 0;
}

void simple_compile(void) {
    kprintf("Simple C Compiler for FelinOS\n");
    kprintf("Supports: int, void, if, else, while, for, return, putchar, puts, getchar\n");
    kprintf("Example:\n");
    kprintf("  int main() { int i = 0; while (i < 10) { putchar(i + 48); i = i + 1; } return 0; }\n\n");

    if (read_source() < 0) return;
    if (compile() < 0) { kprintf("Compilation failed.\n"); return; }

    kprintf("Compiled %d bytes of x86-64 machine code\n", code_size);
    kprintf("Hex dump:\n");
    for (int i = 0; i < code_size; i++) {
        kprintf("%02x ", code[i]);
        if ((i + 1) % 16 == 0) kprintf("\n");
    }
    kprintf("\n");

    typedef int (*func_t)(void);
    func_t fn = (func_t)(uintptr_t)(code + funcs[find_func("main")].addr);
    kprintf("Executing...\n");
    steps = 0; abort_reason = 0;
    stack_base = (uintptr_t)__builtin_frame_address(0);
    int ret = 0;
    if (__builtin_setjmp(run_jb) == 0) {
        ret = fn();
        kprintf("\nDone. main returned %d\n", ret);
    } else {
        static const char *why[] = { "", "step limit exceeded (infinite loop?)",
                                     "stack limit exceeded (runaway recursion?)", "division by zero" };
        kprintf("\nProgram aborted: %s\n", why[abort_reason & 3]);
    }
}

#ifdef CC_HOST_TEST
int main(void) {
    mprotect(code, MAX_CODE, PROT_READ | PROT_WRITE | PROT_EXEC);
    simple_compile();
    return 0;
}
#endif
