/*-
 * Copyright (c) 2024 Jason R. Thorpe.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Assembler for the Tiny BASIC virtual machine.
 *
 * http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html
 *
 * Notes:
 *
 * - To my knowledge, the binary format for the TBVM was never specified
 *   in the original articles.
 *
 * - While the original articles suggest relative labels to keep the
 *   VM byte code more compact, this implementation currently uses
 *   16-bit absolute labels.
 *
 * - This implementation uses 1 byte unsigned literals.
 *
 * - Multibyte values (i.e. labels) are encoded little-endian.
 */

#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "../tbvm/tbvm_opcodes.h"

static bool debug;

static void
dbg_printf(const char *fmt, ...)
{
	va_list ap;

	if (debug) {
		printf("DEBUG: ");
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
}

struct label {
	struct label *next;
	char *string;
	int addr;
	int resolved;		/* line number where declared */
};

static struct label *labels;

struct opcode {
	const char	*str;
	uint8_t		val;
	int		flags;
};

const struct opcode opcode_tab[] = {
	{ "TST",	OPC_TST,	OPC_F_LABEL | OPC_F_STRING },
	{ "CALL",	OPC_CALL,	OPC_F_LABEL },
	{ "RTN",	OPC_RTN,	0 },
	{ "DONE",	OPC_DONE,	0 },
	{ "JMP",	OPC_JMP,	OPC_F_LABEL },
	{ "PRS",	OPC_PRS,	0 },
	{ "PRN",	OPC_PRN,	0 },
	{ "SPC",	OPC_SPC,	0 },
	{ "NLINE",	OPC_NLINE,	0 },
	{ "NXT",	OPC_NXT,	0 },
	{ "XFER",	OPC_XFER,	0 },
	{ "SAV",	OPC_SAV,	0 },
	{ "RSTR",	OPC_RSTR,	0 },
	{ "CMPR",	OPC_CMPR,	0 },
	{ "LIT",	OPC_LIT,	OPC_F_NUMBER },
	{ "INNUM",	OPC_INNUM,	0 },
	{ "FIN",	OPC_FIN,	0 },
	{ "ERR",	OPC_ERR,	0 },
	{ "ADD",	OPC_ADD,	0 },
	{ "SUB",	OPC_SUB,	0 },
	{ "NEG",	OPC_NEG,	0 },
	{ "MUL",	OPC_MUL,	0 },
	{ "DIV",	OPC_DIV,	0 },
	{ "STORE",	OPC_STORE,	0 },
	{ "TSTV",	OPC_TSTV,	OPC_F_LABEL },
	{ "TSTN",	OPC_TSTN,	OPC_F_LABEL },
	{ "IND",	OPC_IND,	0 },
	{ "LST",	OPC_LST,	0 },
	{ "INIT",	OPC_INIT,	0 },
	{ "GETLINE",	OPC_GETLINE,	0 },
	{ "TSTL",	OPC_TSTL,	OPC_F_LABEL },
	{ "INSRT",	OPC_INSRT,	0 },
	{ "XINIT",	OPC_XINIT,	0 },

	{ NULL,		0,		0 },
};

#define	OPC_STATE_FLAGS		(OPC_F_LABEL | OPC_F_STRING | OPC_F_NUMBER)

struct prognode {
	struct prognode *next;
	const struct opcode *opcode;
	struct label *label;
	char *string;
	int number;
	int addr;
	int size;
	int lineno;
};

static struct prognode *program_head;
static struct prognode **program_tailp = &program_head;
static unsigned int current_pc;		/* also program size */
static unsigned int insn_count;
static unsigned int label_count;
static unsigned int labelref_count;

struct parser {
	const char *cursor;
	int lineno;
	int errors;
	int state;

	/* Used to gather info about the current statement. */
	const struct opcode *opcode;
	const char *cp0;
	const char *cp1;
	struct label *label;
	char *string;
	int number;
};

#define	STATE_GET_STATEMENT		0
#define	STATE_GET_STATEMENT_CP0		1
#define	STATE_GET_1OPER_NUM		2
#define	STATE_GET_1OPER_NUM_CP0		3
#define	STATE_GET_1OPER_LABEL		4
#define	STATE_GET_1OPER_LABEL_CP0	5
#define	STATE_GET_2OPER_LABEL		6
#define	STATE_GET_2OPER_LABEL_CP0	7
#define	STATE_GET_2OPER_COMMA		8
#define	STATE_GET_2OPER_STRING		9
#define	STATE_GET_2OPER_STRING_CP0	10
#define	STATE_REST_OF_LINE		11
#define	STATE_COMMENT			12

static void
parser_reset(struct parser * const parser)
{
	parser->state = STATE_GET_STATEMENT,
	parser->opcode = NULL;
	parser->number = -1;
	parser->cp0 = NULL;
	parser->cp1 = NULL;
	parser->label = NULL;
	parser->string = NULL;
}

static inline bool
comment_p(const struct parser * const parser)
{
	return *parser->cursor == ';';
}

static inline bool
whitespace_p(const struct parser * const parser)
{
	const char ch = *parser->cursor;

	return ch == ' ' || ch == '\t';
}

static inline bool
eof_p(const struct parser * const parser)
{
	return *parser->cursor == '\0';
}

static inline bool
newline_p(const struct parser * const parser)
{
	return *parser->cursor == '\n';
}

static inline bool
alpha_p(const struct parser * const parser)
{
	const char ch = *parser->cursor;

	return (ch >= 'A' && ch <= 'Z') ||
	       (ch >= 'a' && ch <= 'z');
}

static inline bool
number_p(const struct parser * const parser)
{
	const char ch = *parser->cursor;

	return ch >= '0' && ch <= '9';
}

static inline bool
alpha_num_p(const struct parser * const parser)
{
	return alpha_p(parser) || number_p(parser);
}

static inline bool
comma_p(const struct parser * const parser)
{
	return *parser->cursor == ',';
}

static inline bool
colon_p(const struct parser * const parser)
{
	return *parser->cursor == ':';
}

static inline bool
squote_p(const struct parser * const parser)
{
	return *parser->cursor == '\'';
}

static void
skip_whitespace(struct parser * const parser)
{
	while (whitespace_p(parser)) {
		parser->cursor++;
	}
}

static size_t
parser_strlen(const struct parser * const parser)
{
	assert(parser->cp0 != NULL);
	assert(parser->cp1 != NULL);
	assert(parser->cp1 >= parser->cp0);

	return parser->cp1 - parser->cp0;
}

static size_t
save_string(struct parser * const parser, char **slot)
{
	size_t len = parser_strlen(parser);
	char *cp = calloc(1, len + 1);
	if (len != 0) {
		memcpy(cp, parser->cp0, len);
	}
	*slot = cp;
	return len;
}

static void
duplicate_label_error(struct parser * const parser,
    const struct label * const l)
{
	fprintf(stderr, "*** duplicate label \"%s\" at line %d\n",
	    l->string, parser->lineno);
	fprintf(stderr, "*** (defined at line %d)\n", l->resolved);
	parser->errors++;
}

static void
invalid_number_error(struct parser * const parser)
{
	fprintf(stderr, "*** invalid number at line %d\n", parser->lineno);
	parser->errors++;
}

static void
invalid_string_error(struct parser * const parser)
{
	fprintf(stderr, "*** invalid string at line %d\n", parser->lineno);
	parser->errors++;
}

static void
invalid_label_error(struct parser * const parser)
{
	fprintf(stderr, "*** invalid label at line %d\n", parser->lineno);
	parser->errors++;
}

static void
syntax_error(struct parser * const parser)
{
	fprintf(stderr, "*** syntax error at line %d\n", parser->lineno);
	parser->errors++;
	parser_reset(parser);		/* discard whatever we've collected */
	parser->state = STATE_COMMENT;	/* discard remainder of line */
}

static void
get_opcode(struct parser * const parser)
{
	const struct opcode *o;
	size_t len = parser_strlen(parser);

	for (o = opcode_tab; o->str != NULL; o++) {
		if (len == strlen(o->str) &&
		    memcmp(parser->cp0, o->str, len) == 0) {
			parser->opcode = o;
			switch (o->flags & OPC_STATE_FLAGS) {
			case 0:
				parser->state = STATE_GET_STATEMENT;
				break;

			case OPC_F_NUMBER:
				parser->state = STATE_GET_1OPER_NUM;
				break;

			case OPC_F_LABEL:
				parser->state = STATE_GET_1OPER_LABEL;
				break;

			case OPC_F_LABEL | OPC_F_STRING:
				parser->state = STATE_GET_2OPER_LABEL;
				break;

			default:
				abort();
			}
			return;
		}
	}
	syntax_error(parser);
}

static void
get_string(struct parser * const parser)
{
	assert(parser->string == NULL);
	if (save_string(parser, &parser->string) == 0) {
		invalid_string_error(parser);
	}
}

static void
get_number(struct parser * const parser)
{
	const char *cp;
	int number, new_number;

	for (number = 0, cp = parser->cp0; cp != parser->cp1; cp++) {
		new_number = (number * 10) + (*cp - '0');
		if (new_number < number || new_number > OPC_NUM_MAX) {
			invalid_number_error(parser);
		}
		number = new_number;
	}
	parser->number = number;
}

static struct label *
gen_label(struct parser * const parser, struct prognode *node)
{
	struct label *l;
	size_t len = parser_strlen(parser);

	for (l = labels; l != NULL; l = l->next) {
		if (len == strlen(l->string) &&
		    memcmp(parser->cp0, l->string, len) == 0) {
			if (node != NULL && l->resolved) {
				duplicate_label_error(parser, l);
				node = NULL;
			}
			break;
		}
	}

	if (l == NULL) {
		l = calloc(1, sizeof(*l));
		if (save_string(parser, &l->string) == 0) {
			/* This should never happen. */
			invalid_label_error(parser);
		}
		l->next = labels;
		labels = l;
	}

	if (node != NULL) {
		l->addr = node->addr;
		l->resolved = node->lineno;
	}

	return l;
}

static struct prognode *
gen_prognode(const struct parser * const parser)
{
	struct prognode *node = calloc(1, sizeof(*node));

	node->addr = current_pc;
	node->lineno = parser->lineno;

	*program_tailp = node;
	program_tailp = &node->next;

	return node;
}

static void
gen_label_decl(struct parser * const parser)
{
	struct prognode *node = gen_prognode(parser);

	node->label = gen_label(parser, node);
	label_count++;

	parser_reset(parser);
}

static void
gen_label_ref(struct parser * const parser)
{
	assert(parser->label == NULL);

	parser->label = gen_label(parser, NULL);
	labelref_count++;
}

static void
gen_insn(struct parser * const parser)
{
	struct prognode *node;

	if (parser->opcode == NULL) {
		return;
	}

	node = gen_prognode(parser);
	node->opcode = parser->opcode;

	node->size = 1;		/* opcode */

	if (node->opcode->flags & OPC_F_NUMBER) {
		node->size += OPC_NUM_SIZE;
		node->number = parser->number;
	} else {
		if (node->opcode->flags & OPC_F_LABEL) {
			assert(parser->label != NULL);
			node->size += OPC_LBL_SIZE;
			node->label = parser->label;
		}
		if (node->opcode->flags & OPC_F_STRING) {
			assert(parser->string != NULL);
			node->size += strlen(parser->string);
			node->string = parser->string;
		}
	}
	current_pc += node->size;
	insn_count++;

	parser_reset(parser);
}

static void
new_line(struct parser * const parser)
{
	/* Emit the instruction we've collected. */
	gen_insn(parser);

	parser->lineno++;
	parser_reset(parser);
}

static inline const char *
plural(int val)
{
	return val == 1 ? "" : "s";
}

static bool
parse(const char *input_buffer)
{
	struct parser parser = {
		.cursor = input_buffer,
		.lineno = 1,
	};
	bool eof = false;

	parser_reset(&parser);

	/*
	 * N.B. the parser's cursor is only advanced if the current
	 * state "consumes" the character at the cursor.  Otherwise,
	 * the byte at the cursor is left for the next state to
	 * act upon.
	 *
	 * The cp1 marker does not count as consuming the byte.  It
	 * serves to mark the first byte AFTER the byte sequence
	 * that starts at cp0.
	 */

	while (!eof) {
		dbg_printf("%s: line %d state %d\n", __func__,
		    parser.lineno, parser.state);
		switch (parser.state) {
		case STATE_GET_STATEMENT:
			skip_whitespace(&parser);
			if (comment_p(&parser)) {
				dbg_printf("%s:     comment\n", __func__);
				parser.state = STATE_COMMENT;
				break;
			}
			if (newline_p(&parser)) {
				dbg_printf("%s:     newline\n", __func__);
				parser.state = STATE_REST_OF_LINE;
				break;
			}
			if (eof_p(&parser)) {
				dbg_printf("%s:     eof\n", __func__);
				eof = true;
				break;
			}
			if (alpha_p(&parser)) {
				dbg_printf("%s:     alpha -> cp0\n", __func__);
				parser.cp0 = parser.cursor++;
				parser.state++;
				break;
			}
			syntax_error(&parser);
			break;

		case STATE_GET_STATEMENT_CP0:
			if (alpha_num_p(&parser)) {
				dbg_printf("%s:     alpha_num\n", __func__);
				parser.cursor++;
				break;
			}
			dbg_printf("%s:     !alpha_num -> cp1\n", __func__);
			parser.cp1 = parser.cursor;
			if (colon_p(&parser)) {
				dbg_printf("%s:     colon -> label decl\n",
				    __func__);
				gen_label_decl(&parser);
				parser.cursor++;
				parser.state = STATE_GET_STATEMENT;
				break;
			}
			get_opcode(&parser);	/* sets next state */
			if (parser.opcode != NULL) {
				dbg_printf("%s:     opcode %s -> %d\n",
				    __func__, parser.opcode->str, parser.state);
			}
			break;

		case STATE_GET_1OPER_NUM:
			skip_whitespace(&parser);
			if (number_p(&parser)) {
				dbg_printf("%s:     number -> cp0\n", __func__);
				parser.cp0 = parser.cursor++;
				parser.state++;
				break;
			}
			syntax_error(&parser);
			break;

		case STATE_GET_1OPER_NUM_CP0:
			if (number_p(&parser)) {
				dbg_printf("%s:     number\n", __func__);
				parser.cursor++;
				break;
			}
			dbg_printf("%s:     !number -> cp1\n", __func__);
			parser.cp1 = parser.cursor;
			get_number(&parser);
			dbg_printf("%s:     got number -> %d\n", __func__,
			    parser.number);
			parser.state = STATE_REST_OF_LINE;
			break;

		case STATE_GET_1OPER_LABEL:
		case STATE_GET_2OPER_LABEL:
			skip_whitespace(&parser);
			if (alpha_p(&parser)) {
				dbg_printf("%s:     alpha -> cp0\n", __func__);
				parser.cp0 = parser.cursor++;
				parser.state++;
				break;
			}
			syntax_error(&parser);
			break;

		case STATE_GET_1OPER_LABEL_CP0:
		case STATE_GET_2OPER_LABEL_CP0:
			if (alpha_num_p(&parser)) {
				dbg_printf("%s:     alpha_num\n", __func__);
				parser.cursor++;
				break;
			}
			dbg_printf("%s:     !alpha_num -> cp1\n", __func__);
			parser.cp1 = parser.cursor;
			gen_label_ref(&parser);
			dbg_printf("%s:     got label ref -> %s\n", __func__,
			    parser.label->string);
			if (parser.state == STATE_GET_1OPER_LABEL_CP0) {
				parser.state = STATE_REST_OF_LINE;
			} else {
				parser.state++;
			}
			break;

		case STATE_GET_2OPER_COMMA:
			skip_whitespace(&parser);
			if (comma_p(&parser)) {
				dbg_printf("%s:     comma\n", __func__);
				parser.cursor++;
				parser.state++;
				break;
			}
			syntax_error(&parser);
			break;

		case STATE_GET_2OPER_STRING:
			skip_whitespace(&parser);
			if (squote_p(&parser)) {
				dbg_printf("%s:     squote\n", __func__);
				parser.cp0 = ++parser.cursor;
				parser.state++;
				break;
			}
			syntax_error(&parser);
			break;

		case STATE_GET_2OPER_STRING_CP0:
			if (!squote_p(&parser)) {
				dbg_printf("%s:     !squote\n", __func__);
				parser.cursor++;
				break;
			}
			dbg_printf("%s:     !squote -> cp1\n", __func__);
			parser.cp1 = parser.cursor;
			parser.cursor++;	/* skip trailing squote */
			get_string(&parser);
			dbg_printf("%s:     got string -> %s\n", __func__,
			    parser.string);
			parser.state = STATE_REST_OF_LINE;
			break;

		case STATE_REST_OF_LINE:
			skip_whitespace(&parser);
			if (comment_p(&parser)) {
				parser.state = STATE_COMMENT;
			} else if (newline_p(&parser)) {
				new_line(&parser);
			} else if (eof_p(&parser)) {
				new_line(&parser);
				eof = true;
			} else {
				syntax_error(&parser);
			}
			parser.cursor++;
			break;

		case STATE_COMMENT:
			if (newline_p(&parser)) {
				new_line(&parser);
			} else if (eof_p(&parser)) {
				new_line(&parser);
				eof = true;
			}
			parser.cursor++;
			break;

		default:
			abort();
		}
	}

	assert(parser.state == STATE_GET_STATEMENT);

	if (parser.errors) {
		fprintf(stderr, "%d error%s parsing input.\n",
		    parser.errors, plural(parser.errors));
		return false;
	} else {
		printf("parsed %u instruction%s (%u label%s, %u reference%s)\n",
		    insn_count, plural(insn_count),
		    label_count, plural(label_count),
		    labelref_count, plural(labelref_count));
	}
	return true;
}

static bool
check_labels(void)
{
	struct prognode *node;
	bool rv = true;

	for (node = program_head; node != NULL; node = node->next) {
		if (node->opcode == NULL ||
		    (node->opcode->flags & OPC_F_LABEL) == 0) {
			continue;
		}
		assert(node->label != NULL);
		if (node->label->resolved == 0) {
			fprintf(stderr,
			   "*** unresolved label refrence \"%s\" at line %d\n",
			   node->label->string, node->lineno);
			rv = false;
		}
	}
	return rv;
}

static char *
generate_program(void)
{
	char *cp, *program;
	struct prognode *node;

	/* current_pc is also the size of the program. */
	if (current_pc == 0) {
		return NULL;
	}
	program = malloc(current_pc);

	for (cp = program, node = program_head; debug && node != NULL;
	     node = node->next) {
		if (node->opcode == NULL) {
			continue;
		}
		printf("%5d: %-10s", node->addr, node->opcode->str);
		if (node->opcode->flags & OPC_F_NUMBER) {
			printf("%d", node->number);
		} else {
			if (node->opcode->flags & OPC_F_LABEL) {
				printf("%d", node->label->addr);
			}
			if (node->opcode->flags & OPC_F_STRING) {
				printf(",'%s'", node->string);
			}
		}
		printf("\n");
	}

	for (cp = program, node = program_head; node != NULL;
	     node = node->next) {
		if (node->opcode == NULL) {
			continue;
		}
		*cp++ = node->opcode->val;
		if (node->opcode->flags & OPC_F_NUMBER) {
			*cp++ = node->number & 0xff;
		} else {
			if (node->opcode->flags & OPC_F_LABEL) {
				/* Absolute, for now. */
				*cp++ = (node->label->addr     ) & 0xff;
				*cp++ = (node->label->addr >> 8) & 0xff;
			}
			if (node->opcode->flags & OPC_F_STRING) {
				size_t len = strlen(node->string);
				memcpy(cp, node->string, len);
				cp[len - 1] |= 0x80; /* terminate string */
				cp += len;
			}
		}
	}
	printf("program size: %u byte%s\n", current_pc, plural(current_pc));

	return program;
}

static char *myprogname;

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-o output.bin] input.asm\n",
	    myprogname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	char *input, *output;
	char *outfname = NULL, *cp;
	char *infname;
	off_t infsize;
	int ch;

	myprogname = strdup(basename(argv[0]));

	while ((ch = getopt(argc, argv, "do:")) != -1) {
		switch (ch) {
		case 'd':
			debug = true;
			break;

		case 'o':
			outfname = strdup(optarg);
			break;

		default:
			usage();
		}
	}
	argv += optind;
	argc -= optind;

	if (argc < 1) {
		usage();
	}
	infname = argv[0];

	if (outfname == NULL) {
		size_t len = strlen(infname) + sizeof(".bin");
		cp = basename(infname);
		outfname = malloc(len);
		strcpy(outfname, infname);
		cp = strrchr(outfname, '.');
		if (cp != NULL) {
			*cp = '\0';
		}
		strcat(outfname, ".bin");
	}

	FILE *infile = fopen(infname, "r");
	if (infile == NULL) {
		fprintf(stderr, "unable to open input file '%s': %s\n",
		    infname, strerror(errno));
		exit(1);
	}

	if (fseeko(infile, 0, SEEK_END) == -1 ||
	    (infsize = ftello(infile)) == -1 ||
	    fseeko(infile, 0, SEEK_SET)) {
		fprintf(stderr, "unable to determine size of '%s': %s\n",
		    infname, strerror(errno));
		exit(1);
	}

	input = malloc((size_t)infsize);
	if (fread(input, (size_t)infsize, 1, infile) != 1) {
		fprintf(stderr, "unable to read input file '%s'\n",
		    infname);
		exit(1);
	}
	fclose(infile);

	if (! parse(input)) {
		exit(1);
	}

	if (! check_labels()) {
		exit(1);
	}

	output = generate_program();

	FILE *outfile = fopen(outfname, "wb");
	if (outfile == NULL) {
		fprintf(stderr, "unable to open output file '%s': %s\n",
		    outfname, strerror(errno));
		exit(1);
	}
	if (fwrite(output, current_pc, 1, outfile) != 1) {
		fprintf(stderr, "unable to write output file '%s'\n",
		    outfname);
		exit(1);
	}
	fclose(outfile);

	exit(0);
}
