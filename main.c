#include "base.h"

#include <sys/mman.h>
#include <unistd.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	u8 *buf;
	u64 len;
	u64 off;
} Arena;

void arena_init(Arena *arena, u8 *buf, u64 len)
{
	arena->buf = buf;
	arena->len = len;
	arena->off = 0;
}

void *arena_alloc_align(Arena *arena, u64 size, u64 align)
{
	assert(align != 0 && (align & (align-1)) == 0);

	uintptr_t base = (uintptr_t)arena->buf;
	uintptr_t ptr = base + (uintptr_t)arena->off;
	ptr = (ptr + align - 1) & ~(align - 1);

	if (ptr + size > base + arena->len) {
		// TODO: Handle OOM.
		assert(0);
	}

	arena->off = ptr - base + size;
	memset((void *)ptr, 0, size);

	return (void *)ptr;
}

#define arena_alloc(arena, n, T) (T *)arena_alloc_align(arena, (n)*sizeof(T), __alignof__(T))

// TODO: Validate with DTD?

typedef u64 TokenType;
enum {
	TOKEN_INVALID,
	TOKEN_EQ,      // =
	TOKEN_LT,      // <
	TOKEN_GT,      // >
	TOKEN_LTQ,     // <?
	TOKEN_QGT,     // ?>
	TOKEN_LTS,     // </
	TOKEN_SGT,     // />
	TOKEN_QUOT,    // "
	TOKEN_APOS,    // '
	TOKEN_NAME,
	TOKEN_NUM,
	TOKEN_ATTRVAL,
	TOKEN_EOF,
};

typedef struct {
	TokenType type;
	String8 str;
} Token;

typedef struct {
	u8 *buf;
	u32 buf_len;
	u32 pos;
} Lexer;

typedef struct {
	Lexer lexer;
} Parser;

typedef struct {
	String8 name;
	String8 value;
} Attribute;

typedef struct {
	// TODO: String-to-string map.
	int foo;
} Attributes;

typedef struct {
	String8 name;
	Attributes attrs;
} Element;

#define between(v, a, b) ((a) <= (v) && (v) <= (b))

static inline b32 is_legal_char(Codepoint p)
{
	u32 v = p.value;
	return v == '\t' || v == '\n' || v == '\r' ||
		between(v, 0x20, 0xD7FF) || between(v, 0xE000, 0xFFFD) || between(v, 0x10000, 0x10FFFF);
}

static inline b32 is_whitespace(Codepoint p)
{
	return p.value == ' ' || p.value == '\t' || p.value == '\r' || p.value == '\n';
}

static inline b32 is_digit(Codepoint p)
{
	return between(p.value, '0', '9');
}

static inline b32 is_name_start_char(Codepoint p)
{
	// TODO: Add rest.
	u32 v = p.value;
	return v == ':' || v == '_' ||
		between(v, 'A', 'Z') || between(v, 'a', 'z');
}

static inline b32 is_name_char(Codepoint p)
{
	u32 v = p.value;
	return is_name_start_char(p) || is_digit(p) || v == '-' || v == '.' ||
		v == 0xB7 || between(v, 0x300, 0x36F) || between(v, 0x203F, 0x2040);
}

static inline Token token_new(TokenType type, u8 *str, u64 size)
{
	String8 lit = {.data=str, .size=size};
	Token token = {.type=type, .str=lit};
	return token;
}

static void lexer_init(Lexer *l, u8 *buf, u32 len)
{
	l->buf = buf;
	l->buf_len = len;
	l->pos = 0;
}

static b32 lexer_eof(Lexer *l)
{
	return l->pos >= l->buf_len;
}

static Codepoint lexer_peek_codepoint(Lexer *l)
{
	assert(!lexer_eof(l));
	return decode_utf8(l->buf+l->pos, l->buf_len-l->pos);
}

static Codepoint lexer_consume_codepoint(Lexer *l)
{
	Codepoint point = lexer_peek_codepoint(l);
	l->pos += point.size;
	return point;
}

static Token lexer_consume_name_token(Lexer *l, u32 pos_start)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_codepoint(l);
		if (!is_name_char(next)) {
			break;
		}
		lexer_consume_codepoint(l);
	}

	return token_new(TOKEN_NAME, l->buf+pos_start, l->pos-pos_start);
}

static Token lexer_consume_num_token(Lexer *l, u32 pos_start)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_codepoint(l);
		if (!is_digit(next) && next.value != '.') {
			break;
		}
		lexer_consume_codepoint(l);
	}

	return token_new(TOKEN_NUM, l->buf+pos_start, l->pos-pos_start);
}

static Token lexer_consume_attr_value(Lexer *l, u32 start, u8 delim)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_codepoint(l);
		if (!is_legal_char(next) || next.value == '<' || next.value == '&')
			return token_new(TOKEN_INVALID, 0, 0);
		if (lexer_consume_codepoint(l).value == delim)
			break;
	}

	String8 lit = str8(l->buf+start, l->pos-start);
	if (lit.size >= 2 && lit.data[lit.size-1] == delim)
		return token_new(TOKEN_ATTRVAL, l->buf+start, l->pos-start);

	return token_new(TOKEN_INVALID, 0, 0);
}

static void lexer_skip_whitespace(Lexer *l)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_codepoint(l);
		if (!is_whitespace(next)) {
			break;
		}
		lexer_consume_codepoint(l);
	}
}

static Token lexer_consume_token(Lexer *l, b32 allow_whitespace)
{
	if (allow_whitespace) {
		lexer_skip_whitespace(l);
	}

	if (lexer_eof(l)) {
		return token_new(TOKEN_EOF, 0, 0);
	}

	Token token = {.type = TOKEN_INVALID};
	u32 pos_start = l->pos;
	Codepoint point, point_next;

	point = lexer_consume_codepoint(l);
	switch (point.value) {
	case '=':
		token = token_new(TOKEN_EQ, l->buf+pos_start, point.size);
		break;
	case '<':
		if (!lexer_eof(l)) {
			point_next = lexer_peek_codepoint(l);
			if (point_next.value == '?') {
				lexer_consume_codepoint(l);
				token = token_new(TOKEN_LTQ, l->buf+pos_start, l->pos-pos_start);
				break;
			} else if (point_next.value == '/') {
				lexer_consume_codepoint(l);
				token = token_new(TOKEN_LTS, l->buf+pos_start, l->pos-pos_start);
				break;
			}
		}
		token = token_new(TOKEN_LT, l->buf+pos_start, point.size);
		break;
	case '>':
		token = token_new(TOKEN_GT, l->buf+pos_start, point.size);
		break;
	case '?':
		if (!lexer_eof(l)) {
			point_next = lexer_peek_codepoint(l);
			if (point_next.value == '>') {
				lexer_consume_codepoint(l);
				token = token_new(TOKEN_QGT, l->buf+pos_start, l->pos-pos_start);
			}
		}
		break;
	case '/':
		if (!lexer_eof(l)) {
			point_next = lexer_peek_codepoint(l);
			if (point_next.value == '>') {
				lexer_consume_codepoint(l);
				token = token_new(TOKEN_SGT, l->buf+pos_start, l->pos-pos_start);
			}
		}
		break;
	case '"':
	case '\'':
		token = lexer_consume_attr_value(l, pos_start, point.value);
		break;
	default:
		if (is_name_start_char(point)) {
			token = lexer_consume_name_token(l, pos_start);
		} else if (is_digit(point)) {
			token = lexer_consume_num_token(l, pos_start);
		}
		break;
	}

	return token;
}

#define fatal(msg) \
	do { \
		write(STDERR_FILENO, (msg), sizeof(msg)-1); \
		exit(1); \
	} while (0)

// Assumes '<' has already been consumed.
static Element *parser_parse_element(Parser *p, Arena *arena)
{
	Token token;

	token = lexer_consume_token(&p->lexer, false);
	if (token.type != TOKEN_NAME) {
		return 0;
	}

	Element *elem = arena_alloc(arena, 1, Element);
	elem->name = token.str;
	// TODO: Initialize elem->attrs.

	do {
		token = lexer_consume_token(&p->lexer, true);
		if (token.type != TOKEN_NAME) {
			break;
		}
		// TODO: Proper attribute parsing.
		token.str; // TODO: attr name
		token = lexer_consume_token(&p->lexer, true);
		assert(token.type == TOKEN_EQ);
		token = lexer_consume_token(&p->lexer, true);
		assert(token.type == TOKEN_ATTRVAL);
	} while (1);

	if (token.type != TOKEN_SGT) {
		return 0;
	}

	return elem;
}

static Element *parser_parse(Parser *p, Arena *arena)
{
	Token token, open, close;

	token = lexer_consume_token(&p->lexer, false);
	if (token.type != TOKEN_LTQ) {
		fatal("expected '<?'\n");
	}

	token = lexer_consume_token(&p->lexer, false);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("xml"))) {
		fatal("expected 'xml'\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("version"))) {
		fatal("expected 'version'\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_EQ) {
		fatal("expected '='\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_ATTRVAL || !str8_eq(token.str, str8_lit("\"1.0\""))) {
		fatal("expected \"1.0\"\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("encoding"))) {
		fatal("expected 'encoding'\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_EQ) {
		fatal("expected '='\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_ATTRVAL || !str8_eq(token.str, str8_lit("\"UTF-8\""))) {
		fatal("expected \"UTF-8\"\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_QGT) {
		fatal("expected '?>'\n");
	}

	token = lexer_consume_token(&p->lexer, true);
	if (token.type != TOKEN_LT) {
		fatal("expected '<'\n");
	}
	Element *root = parser_parse_element(p, arena);
	if (!root) {
		fatal("expected root element\n");
	}

	return root;
}

int main(int argc, char **argv)
{
	u64 memsize = MiB(1);
	void *mem = mmap(0, memsize, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED) {
		fatal("scanner: mmap failed\n");
	}

	Arena arena;
	arena_init(&arena, mem, memsize);

	u32 bufsize = KiB(200);
	u8 *buf = arena_alloc_align(&arena, bufsize, 1);

	ssize_t n = read(STDIN_FILENO, buf, bufsize);
	if (n < 0) {
		munmap(mem, memsize);
		fatal("scanner: read failed\n");
	}

	Parser parser;
	lexer_init(&parser.lexer, buf, n);

	parser_parse(&parser, &arena);

	munmap(mem, memsize);

	return 0;
}
