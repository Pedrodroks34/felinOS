#include "syscall.h"
#include "lib/string.h"
#include "lib/format.h"
#include "console.h"
#include "lib/heap.h"
#include "fs/vfs.h"

#define MAX_TOKENS 1024
#define MAX_SYMS 256
#define MAX_CODE 8192

typedef enum {
    TK_EOF,
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ASSIGN, TK_SEMI, TK_COMMA, TK_LPAREN, TK_RPAREN,
    TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_IF, TK_ELSE, TK_WHILE, TK_RETURN, TK_INT, TK_VOID,
    TK_PUTCHAR, TK_PUTS, TK_GETCHAR,
} TokenType;

struct Token {
    TokenType type;
    char *str;
    int val;
    int line;
};

struct Sym {
    char name[32];
    int offset;
    int is_func;
};

#define EOF (-1)

static char *input_ptr;
static int line_no;
static struct Token tokens[MAX_TOKENS];
static int token_count;
static int token_pos;
static struct Sym syms[MAX_SYMS];
static int sym_count;
static int local_offset;
static uint8_t code[MAX_CODE];
static int code_size;

static int kernel_getchar(void) {
    char c;
    if (vfs_read((struct vfs_file *)1, &c, 1) == 1) {
        return (unsigned char)c;
    }
    return EOF;
}

static void next_char(void) { input_ptr++; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static void skip_whitespace(void) {
    while (*input_ptr) {
        if (*input_ptr == '\n') { line_no++; }
        if (*input_ptr <= ' ') { next_char(); continue; }
        if (*input_ptr == '/' && *(input_ptr + 1) == '/') {
            while (*input_ptr && *input_ptr != '\n') next_char();
            continue;
        }
        break;
    }
}

static void add_token(TokenType type, char *str, int val) {
    if (token_count < MAX_TOKENS) {
        tokens[token_count++] = (struct Token){type, str, val, line_no};
    }
}

static char *read_ident(void) {
    char *start = input_ptr;
    while (is_alnum(*input_ptr)) next_char();
    int len = input_ptr - start;
    char *s = kmalloc(len + 1);
    memcpy(s, start, len);
    s[len] = 0;
    return s;
}

static int read_number(void) {
    int val = 0;
    while (is_digit(*input_ptr)) {
        val = val * 10 + (*input_ptr - '0');
        next_char();
    }
    return val;
}

static void lex(void) {
    input_ptr = (char *)kzalloc(4096);
    kprintf("Enter code (end with Ctrl+D):\n");
    int pos = 0;
    while (1) {
        int c = kernel_getchar();
        if (c == EOF || c == 4) break;
        if (pos < 4095) input_ptr[pos++] = c;
    }
    input_ptr[pos] = 0;
    input_ptr = (char *)input_ptr;
    
    line_no = 1;
    token_count = 0;
    
    while (*input_ptr) {
        skip_whitespace();
        if (!*input_ptr) break;
        
        if (is_alpha(*input_ptr)) {
            char *ident = read_ident();
            if (strcmp(ident, "if") == 0) add_token(TK_IF, ident, 0);
            else if (strcmp(ident, "else") == 0) add_token(TK_ELSE, ident, 0);
            else if (strcmp(ident, "while") == 0) add_token(TK_WHILE, ident, 0);
            else if (strcmp(ident, "return") == 0) add_token(TK_RETURN, ident, 0);
            else if (strcmp(ident, "int") == 0) add_token(TK_INT, ident, 0);
            else if (strcmp(ident, "void") == 0) add_token(TK_VOID, ident, 0);
            else if (strcmp(ident, "putchar") == 0) add_token(TK_PUTCHAR, ident, 0);
            else if (strcmp(ident, "puts") == 0) add_token(TK_PUTS, ident, 0);
            else if (strcmp(ident, "getchar") == 0) add_token(TK_GETCHAR, ident, 0);
            else add_token(TK_IDENT, ident, 0);
        } else if (is_digit(*input_ptr)) {
            int val = read_number();
            add_token(TK_NUMBER, 0, val);
        } else {
            switch (*input_ptr) {
                case '+': add_token(TK_PLUS, 0, 0); break;
                case '-': add_token(TK_MINUS, 0, 0); break;
                case '*': add_token(TK_STAR, 0, 0); break;
                case '/': add_token(TK_SLASH, 0, 0); break;
                case '=':
                    if (*(input_ptr + 1) == '=') { next_char(); next_char(); add_token(TK_EQ, 0, 0); break; }
                    next_char();
                    add_token(TK_ASSIGN, 0, 0);
                    break;
                case '!':
                    if (*(input_ptr + 1) == '=') { next_char(); next_char(); add_token(TK_NEQ, 0, 0); break; }
                    next_char();
                    break;
                case '<':
                    if (*(input_ptr + 1) == '=') { next_char(); next_char(); add_token(TK_LE, 0, 0); break; }
                    next_char();
                    add_token(TK_LT, 0, 0);
                    break;
                case '>':
                    if (*(input_ptr + 1) == '=') { next_char(); next_char(); add_token(TK_GE, 0, 0); break; }
                    next_char();
                    add_token(TK_GT, 0, 0);
                    break;
                case ';': add_token(TK_SEMI, 0, 0); break;
                case ',': add_token(TK_COMMA, 0, 0); break;
                case '(': add_token(TK_LPAREN, 0, 0); break;
                case ')': add_token(TK_RPAREN, 0, 0); break;
                case '{': add_token(TK_LBRACE, 0, 0); break;
                case '}': add_token(TK_RBRACE, 0, 0); break;
                case '[': add_token(TK_LBRACK, 0, 0); break;
                case ']': add_token(TK_RBRACK, 0, 0); break;
                default: next_char(); break;
            }
        }
        if (*input_ptr) next_char();
    }
    add_token(TK_EOF, 0, 0);
}

static struct Token *cur_token(void) {
    return &tokens[token_pos];
}

static void eat(TokenType type) {
    if (cur_token()->type == type) token_pos++;
    else kprintf("Error: expected token %d, got %d at line %d\n", type, cur_token()->type, cur_token()->line);
}

static int find_sym(const char *name) {
    for (int i = 0; i < sym_count; i++) {
        if (strcmp(syms[i].name, name) == 0) return i;
    }
    return -1;
}

static int add_sym(const char *name, int is_func) {
    if (sym_count < MAX_SYMS) {
        strcpy(syms[sym_count].name, name);
        syms[sym_count].offset = local_offset;
        syms[sym_count].is_func = is_func;
        if (!is_func) local_offset += 4;
        return sym_count++;
    }
    return -1;
}

static void emit_byte(uint8_t b) {
    if (code_size < MAX_CODE) code[code_size++] = b;
}

static void emit_int(uint32_t v) {
    emit_byte(v & 0xFF);
    emit_byte((v >> 8) & 0xFF);
    emit_byte((v >> 16) & 0xFF);
    emit_byte((v >> 24) & 0xFF);
}

static void gen_expr(void);
static void gen_stmt(void);

static void gen_expr(void) {
    if (cur_token()->type == TK_NUMBER) {
        emit_byte(0xB8); // mov eax, imm32
        emit_int(cur_token()->val);
        token_pos++;
    } else if (cur_token()->type == TK_IDENT) {
        int idx = find_sym(cur_token()->str);
        if (idx >= 0) {
            if (syms[idx].is_func) {
                token_pos++;
                eat(TK_LPAREN);
                gen_expr();
                eat(TK_RPAREN);
                emit_byte(0xE8); // call rel32
                emit_int(syms[idx].offset - code_size - 4);
            } else {
                emit_byte(0x8B); emit_byte(0x45); // mov eax, [ebp+offset]
                emit_byte(syms[idx].offset);
                token_pos++;
            }
        }
    } else if (cur_token()->type == TK_LPAREN) {
        token_pos++;
        gen_expr();
        eat(TK_RPAREN);
    }
    
    while (1) {
        TokenType op = cur_token()->type;
        if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR || op == TK_SLASH ||
            op == TK_EQ || op == TK_NEQ || op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE) {
            token_pos++;
            gen_expr();
            code_size -= 4;
            emit_byte(0x50); // push eax
            switch (op) {
                case TK_PLUS: emit_byte(0x01); emit_byte(0xC0); break; // add eax, [esp]
                case TK_MINUS: emit_byte(0x29); emit_byte(0xC0); break; // sub eax, [esp]
                case TK_STAR: emit_byte(0xF7); emit_byte(0x64); emit_byte(0x24); emit_byte(0x00); break; // imul eax, [esp]
                case TK_SLASH: emit_byte(0x99); emit_byte(0xF7); emit_byte(0x7C); emit_byte(0x24); emit_byte(0x00); break; // idiv [esp]
                case TK_EQ: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x94); emit_byte(0xC0); break; // cmp + sete
                case TK_NEQ: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC0); break;
                case TK_LT: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x9C); emit_byte(0xC0); break;
                case TK_LE: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x9E); emit_byte(0xC0); break;
                case TK_GT: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x9F); emit_byte(0xC0); break;
                case TK_GE: emit_byte(0x39); emit_byte(0x44); emit_byte(0x24); emit_byte(0x00); emit_byte(0x0F); emit_byte(0x9D); emit_byte(0xC0); break;
                /* The guard above admits nothing else, so nothing else can
                 * reach here; the case exists to tell the compiler so. */
                default: break;
            }
            emit_byte(0x83); emit_byte(0xC4); emit_byte(0x04); // add esp, 4
        } else break;
    }
}

static void gen_stmt(void) {
    if (cur_token()->type == TK_IF) {
        token_pos++;
        eat(TK_LPAREN);
        gen_expr();
        eat(TK_RPAREN);
        emit_byte(0x85); emit_byte(0xC0); // test eax, eax
        int jz_pos = code_size;
        emit_byte(0x0F); emit_byte(0x84); // jz rel32
        emit_int(0); // placeholder
        gen_stmt();
        if (cur_token()->type == TK_ELSE) {
            int jmp_pos = code_size;
            emit_byte(0xE9); // jmp rel32
            emit_int(0);
            uint32_t *patch = (uint32_t *)&code[jz_pos + 2];
            *patch = code_size - (jz_pos + 6);
            token_pos++;
            gen_stmt();
            patch = (uint32_t *)&code[jmp_pos + 1];
            *patch = code_size - (jmp_pos + 5);
        } else {
            uint32_t *patch = (uint32_t *)&code[jz_pos + 2];
            *patch = code_size - (jz_pos + 6);
        }
    } else if (cur_token()->type == TK_WHILE) {
        token_pos++;
        int loop_start = code_size;
        eat(TK_LPAREN);
        gen_expr();
        eat(TK_RPAREN);
        emit_byte(0x85); emit_byte(0xC0);
        int jz_pos = code_size;
        emit_byte(0x0F); emit_byte(0x84);
        emit_int(0);
        gen_stmt();
        emit_byte(0xE9); // jmp
        emit_int(loop_start - code_size - 4);
        uint32_t *patch = (uint32_t *)&code[jz_pos + 2];
        *patch = code_size - (jz_pos + 6);
    } else if (cur_token()->type == TK_RETURN) {
        token_pos++;
        gen_expr();
        emit_byte(0xC9); // leave
        emit_byte(0xC3); // ret
        eat(TK_SEMI);
    } else if (cur_token()->type == TK_IDENT) {
        char *name = cur_token()->str;
        token_pos++;
        if (cur_token()->type == TK_ASSIGN) {
            int idx = find_sym(name);
            token_pos++;
            gen_expr();
            if (idx >= 0) {
                emit_byte(0x89); emit_byte(0x45); // mov [ebp+offset], eax
                emit_byte(syms[idx].offset);
            }
            eat(TK_SEMI);
        } else if (cur_token()->type == TK_LPAREN) {
            int idx = find_sym(name);
            token_pos++;
            if (cur_token()->type != TK_RPAREN) {
                gen_expr();
            }
            eat(TK_RPAREN);
            eat(TK_SEMI);
            if (idx >= 0 && syms[idx].is_func) {
                emit_byte(0xE8);
                emit_int(syms[idx].offset - code_size - 4);
            } else if (strcmp(name, "putchar") == 0) {
                emit_byte(0x50); // push eax
                emit_byte(0xB8); emit_int(SYS_WRITE); // mov eax, SYS_WRITE
                emit_byte(0xBB); emit_int(1); // mov ebx, 1 (stdout)
                emit_byte(0x89); emit_byte(0xE1); // mov ecx, esp
                emit_byte(0xBA); emit_int(1); // mov edx, 1
                emit_byte(0xCD); emit_byte(0x80); // int 0x80
                emit_byte(0x83); emit_byte(0xC4); emit_byte(0x04); // add esp, 4
            } else if (strcmp(name, "puts") == 0) {
                emit_byte(0x50);
                emit_byte(0xB8); emit_int(SYS_WRITE);
                emit_byte(0xBB); emit_int(1);
                emit_byte(0x89); emit_byte(0xE1);
                emit_byte(0x8B); emit_byte(0x10); // mov edx, [eax]
                emit_byte(0xCD); emit_byte(0x80);
                emit_byte(0x83); emit_byte(0xC4); emit_byte(0x04);
            } else if (strcmp(name, "getchar") == 0) {
                emit_byte(0xB8); emit_int(SYS_READ);
                emit_byte(0xBB); emit_int(0); // stdin
                emit_byte(0x89); emit_byte(0xE1); // mov ecx, esp
                emit_byte(0xBA); emit_int(1);
                emit_byte(0xCD); emit_byte(0x80);
            }
        }
    } else if (cur_token()->type == TK_LBRACE) {
        token_pos++;
        while (cur_token()->type != TK_RBRACE && cur_token()->type != TK_EOF) {
            gen_stmt();
        }
        eat(TK_RBRACE);
    } else if (cur_token()->type == TK_INT) {
        token_pos++;
        char *name = cur_token()->str;
        token_pos++;
        if (cur_token()->type == TK_ASSIGN) {
            token_pos++;
            gen_expr();
            add_sym(name, 0);
            emit_byte(0x89); emit_byte(0x45);
            emit_byte(syms[sym_count - 1].offset);
        } else {
            add_sym(name, 0);
        }
        eat(TK_SEMI);
    } else if (cur_token()->type == TK_VOID) {
        token_pos++;
        if (cur_token()->type == TK_IDENT) {
            char *name = cur_token()->str;
            token_pos++;
            eat(TK_LPAREN);
            eat(TK_RPAREN);
            eat(TK_LBRACE);
            int idx = add_sym(name, 1);
            syms[idx].offset = code_size;
            emit_byte(0x55); // push ebp
            emit_byte(0x89); emit_byte(0xE5); // mov ebp, esp
            emit_byte(0x83); emit_byte(0xEC); emit_byte(local_offset & 0xFF); // sub esp, local_offset
            while (cur_token()->type != TK_RBRACE && cur_token()->type != TK_EOF) {
                gen_stmt();
            }
            eat(TK_RBRACE);
            emit_byte(0xC9); // leave
            emit_byte(0xC3); // ret
            local_offset = 0;
            sym_count = 0;
        }
    } else if (cur_token()->type == TK_SEMI) {
        token_pos++;
    }
}

void simple_compile(void) {
    kprintf("Simple C Compiler for FelinOS\n");
    kprintf("Supports: int, void, if, else, while, return, putchar, puts, getchar\n");
    kprintf("Example:\n");
    kprintf("  int main() { int i = 0; while (i < 10) { putchar(i + 48); i = i + 1; } return 0; }\n\n");
    
    lex();
    token_pos = 0;
    local_offset = 0;
    sym_count = 0;
    code_size = 0;
    
    while (cur_token()->type != TK_EOF) {
        gen_stmt();
    }
    
    kprintf("Compiled %d bytes of x86 machine code\n", code_size);
    kprintf("Hex dump:\n");
    for (int i = 0; i < code_size; i++) {
        kprintf("%02x ", code[i]);
        if ((i + 1) % 16 == 0) kprintf("\n");
    }
    kprintf("\n");
    
    // Execute the code
    if (code_size > 0) {
        kprintf("Executing...\n");
        typedef void (*func_t)(void);
        func_t f = (func_t)code;
        f();
        kprintf("\nDone.\n");
    }
}