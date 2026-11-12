#include "base.h"

#include <sys/mman.h>
#include <unistd.h>

#include <stdlib.h>
#include <stdio.h>

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
	TOKEN_LTBHH,   // <!--
	TOKEN_HHGT,    // -->
	TOKEN_QUOT,    // "
	TOKEN_APOS,    // '
	TOKEN_WS,      // ' ', '\t', '\r', '\n'
	TOKEN_NAME,
	TOKEN_ATTRVAL,
	TOKEN_EOF,
};

typedef struct {
	TokenType type;
	String8   str;
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

// string-to-string map
typedef struct {
	Attribute *arr;
	u64 len;
} Attributes;

typedef struct Element Element;

typedef struct {
	Element *head;
	Element *tail;
} ElementList;

struct Element {
	String8    name;
	Attributes attrs;

	ElementList children;
	Element *next;
};

#define sll_pushback(head, tail, node) \
	do { \
		if (!(head)) { \
			(head) = (tail) = (node); \
		} else { \
			(tail)->next = (node); \
			(tail) = (node); \
		} \
	} while (0)

static Attributes attrs_new(Arena *arena, u64 len)
{
	Attributes attrs;
	attrs.arr = arena_alloc(arena, len, Attribute);
	attrs.len = len;
	return attrs;
}

// TODO: Is this hash function good enough?
static u64 hash(String8 s)
{
	u64 hash = 5381;
	for (u64 i = 0; i < s.size; ++i)
		hash = hash*33 ^ s.data[i];
	return hash;
}

static Attribute *attrs_get(Attributes attrs, String8 name)
{
	u64 i = hash(name) % attrs.len;
	return attrs.arr + i;
}

static void attrs_set(Attributes attrs, String8 name, String8 value)
{
	Attribute *attr = attrs_get(attrs, name);

	// TODO: collisions and resizing
	assert(attr->name.data == 0);

	attr->name = name;
	attr->value = value;
}

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
	u32 v = p.value;
	return v == ':' || v == '_' ||
		between(v, 'A', 'Z') || between(v, 'a', 'z') ||
		between(v, 0xC0, 0xD6) || between(v, 0xD8, 0xF6) ||
		between(v, 0xF8, 0x2FF) || between(v, 0x370, 0x37D) ||
		between(v, 0x37F, 0x1FFF) || between(v, 0x200C, 0x200D) ||
		between(v, 0x2070, 0x218F) || between(v, 0x2C00, 0x2FEF) ||
		between(v, 0x3001, 0xD7FF) || between(v, 0xF900, 0xFDCF) ||
		between(v, 0xFDF0, 0xFFFD) || between(v, 0x10000, 0xEFFFF);
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

static Codepoint lexer_peek_char(Lexer *l)
{
	assert(!lexer_eof(l));
	return decode_utf8(l->buf+l->pos, l->buf_len-l->pos);
}

static Codepoint lexer_consume_char(Lexer *l)
{
	Codepoint point = lexer_peek_char(l);
	l->pos += point.size;
	return point;
}

// Assume name start char has been consumed.
static Token lexer_consume_name_token(Lexer *l, u32 pos_start)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_char(l);
		if (!is_name_char(next))
			break;
		lexer_consume_char(l);
	}

	return token_new(TOKEN_NAME, l->buf+pos_start, l->pos-pos_start);
}

static Token lexer_consume_attr_value(Lexer *l, u32 start, u8 delim)
{
	while (!lexer_eof(l)) {
		Codepoint next = lexer_peek_char(l);
		if (!is_legal_char(next) || next.value == '<' || next.value == '&')
			return token_new(TOKEN_INVALID, 0, 0);
		if (lexer_consume_char(l).value == delim)
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
		Codepoint next = lexer_peek_char(l);
		if (!is_whitespace(next))
			break;
		lexer_consume_char(l);
	}
}

static Token lexer_consume_token(Lexer *l)
{
	if (lexer_eof(l)) {
		return token_new(TOKEN_EOF, 0, 0);
	}

	Token token = {.type = TOKEN_INVALID};
	u32 pos_start = l->pos;
	Codepoint ch, next;

	ch = lexer_consume_char(l);
	switch (ch.value) {
	case '=':
		token = token_new(TOKEN_EQ, l->buf+pos_start, ch.size);
		break;
	case '<':
		if (!lexer_eof(l)) {
			next = lexer_peek_char(l);
			if (next.value == '?') {
				lexer_consume_char(l);
				token = token_new(TOKEN_LTQ, l->buf+pos_start, l->pos-pos_start);
				break;
			} else if (next.value == '/') {
				lexer_consume_char(l);
				token = token_new(TOKEN_LTS, l->buf+pos_start, l->pos-pos_start);
				break;
			}
		}
		token = token_new(TOKEN_LT, l->buf+pos_start, ch.size);
		break;
	case '>':
		token = token_new(TOKEN_GT, l->buf+pos_start, ch.size);
		break;
	case '?':
		if (!lexer_eof(l)) {
			next = lexer_peek_char(l);
			if (next.value == '>') {
				lexer_consume_char(l);
				token = token_new(TOKEN_QGT, l->buf+pos_start, l->pos-pos_start);
			}
		}
		break;
	case '/':
		if (!lexer_eof(l)) {
			next = lexer_peek_char(l);
			if (next.value == '>') {
				lexer_consume_char(l);
				token = token_new(TOKEN_SGT, l->buf+pos_start, l->pos-pos_start);
			}
		}
		break;
	case '"':
	case '\'':
		token = lexer_consume_attr_value(l, pos_start, ch.value);
		break;
	default:
		if (is_name_start_char(ch)) {
			token = lexer_consume_name_token(l, pos_start);
		} else if (is_whitespace(ch)) {
			token = token_new(TOKEN_WS, l->buf+pos_start, ch.size);
		}
		break;
	}

	return token;
}

// All text that is not markup is character data.
static String8 lexer_consume_chardata(Lexer *l, u32 start)
{
	// TODO: Handle CDATA close delimiter.

	while (!lexer_eof(l)) {
		Codepoint ch = lexer_peek_char(l);
		if (!is_legal_char(ch) || ch.value == '<' || ch.value == '&')
			break;
		lexer_consume_char(l);
	}

	return str8(l->buf+start, l->pos-start);
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

	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_NAME) {
		return 0;
	}

	Element *elem = arena_alloc(arena, 1, Element);
	elem->name = token.str;
	elem->attrs = attrs_new(arena, 32);

	while (1) {
		lexer_skip_whitespace(&p->lexer);
		token = lexer_consume_token(&p->lexer);
		if (token.type != TOKEN_NAME) {
			break;
		}

		String8 name = token.str;

		lexer_skip_whitespace(&p->lexer);
		token = lexer_consume_token(&p->lexer);
		assert(token.type == TOKEN_EQ);

		lexer_skip_whitespace(&p->lexer);
		token = lexer_consume_token(&p->lexer);
		assert(token.type == TOKEN_ATTRVAL);

		String8 value = token.str;

		attrs_set(elem->attrs, name, value);
	};

	switch (token.type) {
	case TOKEN_SGT:
		return elem;
	case TOKEN_GT:
		while (1) {
			// TODO: Whitespace handling. Right now, leading character data
			// whitespace isn't preserved.
			u32 start = p->lexer.pos;
			lexer_skip_whitespace(&p->lexer);
			token = lexer_consume_token(&p->lexer);
			if (token.type == TOKEN_LTS) {
				break;
			} else if (token.type == TOKEN_LT) {
				// TODO: Build tree.
				Element *child = parser_parse_element(p, arena);
				if (child) {
					sll_pushback(elem->children.head, elem->children.tail, child);
				}
			} else if (token.type == TOKEN_LTBHH) {
				// TODO: Handle comments.
			} else {
				// Assume character data for now.
				lexer_consume_chardata(&p->lexer, start);
			}
			// TODO: Handle other non character data.
		};
		token = lexer_consume_token(&p->lexer);
		assert(token.type == TOKEN_NAME && str8_eq(token.str, elem->name));
		lexer_skip_whitespace(&p->lexer);
		token = lexer_consume_token(&p->lexer);
		assert(token.type == TOKEN_GT);
		return elem;
	default:
		return 0;
	}
}

static Element *parser_parse(Parser *p, Arena *arena)
{
	Token token;

	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_LTQ) {
		fatal("expected '<?'\n");
	}

	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("xml"))) {
		fatal("expected 'xml'\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("version"))) {
		fatal("expected 'version'\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_EQ) {
		fatal("expected '='\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_ATTRVAL || !str8_eq(token.str, str8_lit("\"1.0\""))) {
		fatal("expected \"1.0\"\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_NAME || !str8_eq(token.str, str8_lit("encoding"))) {
		fatal("expected 'encoding'\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_EQ) {
		fatal("expected '='\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_ATTRVAL || !str8_eq(token.str, str8_lit("\"UTF-8\""))) {
		fatal("expected \"UTF-8\"\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_QGT) {
		fatal("expected '?>'\n");
	}

	lexer_skip_whitespace(&p->lexer);
	token = lexer_consume_token(&p->lexer);
	if (token.type != TOKEN_LT) {
		fatal("expected '<'\n");
	}
	Element *root = parser_parse_element(p, arena);
	if (!root) {
		fatal("expected root element\n");
	}

	return root;
}

static void treewalk(Element *e, s32 lvl)
{
	if (!e) return;

	for (s32 i = 0; i < lvl; ++i)
		printf("    ");
	printf("<%.*s", (int)e->name.size, e->name.data);
	for (u32 i = 0; i < e->attrs.len; ++i) {
		if (e->attrs.arr[i].name.data) {
			printf(" %.*s=%.*s",
					(int)e->attrs.arr[i].name.size, e->attrs.arr[i].name.data,
					(int)e->attrs.arr[i].value.size, e->attrs.arr[i].value.data);
		}
	}
	printf(">\n");

	for (Element *c = e->children.head; c; c = c->next) {
		treewalk(c, lvl+1);
	}

	for (s32 i = 0; i < lvl; ++i)
		printf("    ");
	printf("</%.*s>\n", (int)e->name.size, e->name.data);
}

int main(int argc, char **argv)
{
	u64 memsize = MiB(4);
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

	Element *root = parser_parse(&parser, &arena);
	treewalk(root, 0);

	munmap(mem, memsize);

	return 0;
}
