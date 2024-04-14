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
 * An implementation of the Tiny BASIC Virtual Machine.
 *
 * http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html
 *
 * Note that this implementation uses 2-byte absolute label references
 * that are little-endian encoded.  The original specification recommended
 * relative label references for space savings; that's not a huge concern
 * in this implementation, and the extra breathing room makes it easier
 * to extend the BASIC interpreter.
 */

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "tbvm.h"
#include "tbvm_opcodes.h"

#define	DOES_NOT_RETURN	__attribute__((__noreturn__))

#define	NUM_VARS	26	/* A - Z */
#define	SIZE_CSTK	64	/* control stack size */
#define	SIZE_SBRSTK	64	/* subroutine stack size */
#define	SIZE_AESTK	64	/* expression stack size */
#define	SIZE_LPSTK	NUM_VARS/* loop stack size */
#define	SIZE_LBUF	256

#define	MAX_LINENO	65535	/* arbitrary */

#define	DQUOTE		'"'
#define	END_OF_LINE	'\n'

struct saveloc {
	int lineno;
	int lbuf_ptr;
};

struct loop {
	int var;
	int lineno;
	int start_val;
	int end_val;
	int step;
};

typedef struct string {
	unsigned int refs;
	struct string *next;
	char *str;
	size_t len;
} string;

struct value {
	int type;
	union {
		int	integer;
		string *string;
	};
};
#define	VALUE_TYPE_ANY		0
#define	VALUE_TYPE_INTEGER	1
#define	VALUE_TYPE_STRING	2
#define	VALUE_TYPE_VARREF	10

static bool
value_valid_p(const struct value *value)
{
	bool rv = true;

	switch (value->type) {
	case VALUE_TYPE_INTEGER:
		break;

	case VALUE_TYPE_STRING:
		rv = (value->string != NULL);
		break;

	case VALUE_TYPE_VARREF:
		rv = (value->integer >= 0 && value->integer <= NUM_VARS);
		break;

	default:
		rv = false;
	}

	return rv;
}

struct tbvm {
	jmp_buf		vm_abort_env;
	jmp_buf		basic_error_env;
	sig_atomic_t	break_received;

	const char	*vm_prog;
	size_t		vm_progsize;
	bool		vm_run;
	unsigned int	pc;	/* VM program counter */
	unsigned int	opc_pc;	/* VM program counter of current opcode */
	unsigned char	opc;	/* current opcode */

	unsigned int	collector_pc;	/* VM address of collector routine */
	unsigned int	executor_pc;	/* VM address of executor routine */

	bool		suppress_prompt;
	bool		direct;		/* true if in DIRECT mode */
	int		lineno;		/* current BASIC line number */
	int		first_line;
	int		last_line;
	char		*progstore[MAX_LINENO];

	string		*strings;
	bool		strings_need_gc;

	void		*context;
	int		(*io_getchar)(void *);
	void		(*io_putchar)(void *, int);

	struct value	vars[NUM_VARS];

	char		direct_lbuf[SIZE_LBUF];

	char		*lbuf;
	int		lbuf_ptr;

	int		cstk[SIZE_CSTK];
	int		cstk_ptr;

	struct saveloc	sbrstk[SIZE_SBRSTK];
	int		sbrstk_ptr;

	struct value	aestk[SIZE_AESTK];
	int		aestk_ptr;

	struct loop	lpstk[SIZE_LPSTK];
	int		lpstk_ptr;
};

static string *
string_alloc(tbvm *vm, const char *str, size_t len)
{
	string *string = malloc(sizeof(*string));
	string->str = malloc(len + 1);
	memcpy(string->str, str, len);
	string->str[len] = '\0';
	string->refs = 1;

	string->next = vm->strings;
	vm->strings = string;

	return string;
}

static void
string_free(tbvm *vm, string *string)
{
	free(string->str);
	free(string);
}

static void
string_retain(tbvm *vm, string *string)
{
	string->refs++;
	assert(string->refs != 0);
}

static void
string_release(tbvm *vm, string *string)
{
	assert(string->refs != 0);
	string->refs--;
	if (string->refs == 0) {
		vm->strings_need_gc = true;
	}
}

static void
string_gc(tbvm *vm)
{
	if (vm->strings_need_gc) {
		string *string, *next, **nextp;

		for (string = vm->strings, nextp = &vm->strings;
		     string != NULL;
		     string = next) {
			next = string->next;
			if (string->refs == 0) {
				*nextp = next;
				string_free(vm, string);
			} else {
				nextp = &string->next;
			}
		}
		vm->strings_need_gc = false;
	}
}

static void
string_freeall(tbvm *vm)
{
	string *string;

	while ((string = vm->strings) != NULL) {
		vm->strings = string->next;
		string_free(vm, string);
	}
}

static void
print_crlf(tbvm *vm)
{
	(*vm->io_putchar)(vm->context, '\n');
}

static void
print_cstring(tbvm *vm, const char *msg)
{
	const char *cp;

	for (cp = msg; *cp != '\0'; cp++) {
		(*vm->io_putchar)(vm->context, *cp);
	}
}

static int
printed_number_width(int num)
{
	bool negative_p = false;
	int width;

	if (num == 0) {
		return 1;
	}
	if (num < 0) {
		negative_p = true;
		num = -num;
	}
	for (width = 0; num != 0; num /= 10) {
		width++;
	}
	if (negative_p) {
		width++;
	}
	return width;
}

static void
print_number_justified(tbvm *vm, int num, int width)
{
	/*
	 * Largest integer number is "2147483647", which is 10
	 * digits.  Allow an extra byte for "-" in case it's
	 * negative.
	 */
#define	PRN_BUFSIZE	11
	bool negative_p = num < 0;
	char buf[PRN_BUFSIZE];
	char *cp = &buf[PRN_BUFSIZE];
	int digits = 0;

	if (negative_p) {
		num = -num;
	}

	do {
		*--cp = '0' + (num % 10);
		num /= 10;
		digits++;
	} while (num != 0);

	if (negative_p) {
		*--cp = '-';
		digits++;
	}

	if (width != 0) {
		if (width > PRN_BUFSIZE) {
			width = PRN_BUFSIZE;
		}
		while (digits < width) {
			*--cp = ' ';
			digits++;
		}
	}

	do {
		(*vm->io_putchar)(vm->context, *cp++);
	} while (cp != &buf[PRN_BUFSIZE]);
}

static void
print_number(tbvm *vm, int num)
{
	print_number_justified(vm, num, 0);
}

static void DOES_NOT_RETURN
vm_abort(tbvm *vm, const char *msg)
{
	print_cstring(vm, msg);
	print_cstring(vm, ", PC=");
	print_number(vm, vm->opc_pc);
	print_cstring(vm, ", OPC=");
	print_number(vm, vm->opc);
	print_crlf(vm);
	vm->vm_run = false;
	longjmp(vm->vm_abort_env, 1);
}

static void
direct_mode(tbvm *vm, int ptr)
{
	vm->direct = true;
	vm->pc = vm->collector_pc;
	vm->lineno = 0;
	vm->lbuf = vm->direct_lbuf;
	vm->lbuf_ptr = ptr;
}

/*********** BASIC error helper routines **********/

static void DOES_NOT_RETURN
basic_error(tbvm *vm, const char *msg)
{
	print_cstring(vm, msg);
	if (! vm->direct) {
		print_cstring(vm, " AT LINE ");
		print_number(vm, vm->lineno);
	}
	print_crlf(vm);
	longjmp(vm->basic_error_env, 1);
}

static void DOES_NOT_RETURN
basic_syntax_error(tbvm *vm)
{
	basic_error(vm, "?SYNTAX ERROR");
}

static void DOES_NOT_RETURN
basic_missing_line_error(tbvm *vm)
{
	basic_error(vm, "?MISSING LINE");
}

static void DOES_NOT_RETURN
basic_line_number_error(tbvm *vm)
{
	basic_error(vm, "?LINE NUMBER OUT OF RANGE");
}

static void DOES_NOT_RETURN
basic_gosub_error(tbvm *vm)
{
	basic_error(vm, "?TOO MANY GOSUBS");
}

static void DOES_NOT_RETURN
basic_return_error(tbvm *vm)
{
	basic_error(vm, "?RETURN WITHOUT GOSUB");
}

static void DOES_NOT_RETURN
basic_for_error(tbvm *vm)
{
	basic_error(vm, "?TOO MANY FOR LOOPS");
}

static void DOES_NOT_RETURN
basic_step_error(tbvm *vm)
{
	basic_error(vm, "?BAD STEP");
}

static void DOES_NOT_RETURN
basic_next_error(tbvm *vm)
{
	basic_error(vm, "?NEXT WITHOUT FOR");
}

static void DOES_NOT_RETURN
basic_expression_error(tbvm *vm)
{
	basic_error(vm, "?EXPRESSION TOO COMPLEX");
}

#if 0
static void DOES_NOT_RETURN
basic_too_many_lines_error(tbvm *vm)
{
	basic_error(vm, "?TOO MANY LINES");
}
#endif

static void DOES_NOT_RETURN
basic_division_by_zero_error(tbvm *vm)
{
	basic_error(vm, "?DIVISION BY ZERO");
}

static void DOES_NOT_RETURN
basic_number_range_error(tbvm *vm)
{
	basic_error(vm, "?NUMBER OUT OF RANGE");
}

static void DOES_NOT_RETURN
basic_wrong_type_error(tbvm *vm)
{
	basic_error(vm, "?WRONG VALUE TYPE");
}

/*********** Generic stack routines **********/

static int
stack_push(int *stkptr, int stksize)
{
	int rv;

	if (*stkptr == stksize) {
		rv = -1;
	} else {
		rv = *stkptr;
		*stkptr = rv + 1;
	}

	return rv;
}

static int
stack_pop(int *stkptr, int stksize)
{
	int rv;

	if (*stkptr == 0) {
		rv = -1;
	} else {
		rv = *stkptr - 1;
		*stkptr = rv;
	}

	return rv;
}

/*********** Control stack routines **********/

static void
cstk_push(tbvm *vm, int val)
{
	int slot;

	if ((slot = stack_push(&vm->cstk_ptr, SIZE_CSTK)) == -1) {
		vm_abort(vm, "!CONTROL STACK OVERFLOW");
	}
	
	vm->cstk[slot] = val;
}

static int
cstk_pop(tbvm *vm)
{
	int slot;

	if ((slot = stack_pop(&vm->cstk_ptr, SIZE_CSTK)) == -1) {
		vm_abort(vm, "!CONTROL STACK UNDERFLOW");
	}

	return vm->cstk[slot];
}

/*********** Subroutine stack routines **********/

static void
sbrstk_push(tbvm *vm, int line, int ptr)
{
	int slot;

	if ((slot = stack_push(&vm->sbrstk_ptr, SIZE_SBRSTK)) == -1) {
		basic_gosub_error(vm);
	}
	vm->sbrstk[slot].lineno = line;
	vm->sbrstk[slot].lbuf_ptr = ptr;
}

static void
sbrstk_pop(tbvm *vm, int *linep, int *ptrp)
{
	int slot;

	if ((slot = stack_pop(&vm->sbrstk_ptr, SIZE_SBRSTK)) == -1) {
		basic_return_error(vm);
	}
	*linep = vm->sbrstk[slot].lineno;
	*ptrp = vm->sbrstk[slot].lbuf_ptr;
}

/*********** Arithmetic Expression stack routines **********/

static void
aestk_push_value(tbvm *vm, const struct value *valp)
{
	int slot;

	if (! value_valid_p(valp)) {
		vm_abort(vm, "!PUSHING INVALID VALUE");
	}

	if ((slot = stack_push(&vm->aestk_ptr, SIZE_AESTK)) == -1) {
		basic_expression_error(vm);
	}
	vm->aestk[slot] = *valp;
}

static void
aestk_pop_value(tbvm *vm, int type, struct value *valp)
{
	int slot;

	if ((slot = stack_pop(&vm->aestk_ptr, SIZE_AESTK)) == -1) {
		vm_abort(vm, "!EXPRESSION STACK UNDERFLOW");
	}
	*valp = vm->aestk[slot];
	if (type != VALUE_TYPE_ANY && type != valp->type) {
		basic_wrong_type_error(vm);
	}
}

static void
aestk_push_integer(tbvm *vm, int val)
{
	struct value value = {
		.type = VALUE_TYPE_INTEGER,
		.integer = val,
	};
	aestk_push_value(vm, &value);
}

static int
aestk_pop_integer(tbvm *vm)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_INTEGER, &value);
	return value.integer;
}

static void
aestk_push_varref(tbvm *vm, int var)
{
	struct value value = {
		.type = VALUE_TYPE_VARREF,
		.integer = var,
	};
	aestk_push_value(vm, &value);
}

static int
aestk_pop_varref(tbvm *vm)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_VARREF, &value);
	return value.integer;
}

/*********** Loop stack routines **********/

static void
lpstk_push(tbvm *vm, const struct loop *l)
{
	int slot;

	if ((slot = stack_push(&vm->lpstk_ptr, SIZE_LPSTK)) == -1) {
		basic_for_error(vm);
	}
	vm->lpstk[slot] = *l;
}

static struct loop *
lpstk_peek_top(tbvm *vm)
{
	if (vm->lpstk_ptr == 0) {
		vm_abort(vm, "!LOOP STACK EMPTY");
	}
	return &vm->lpstk[vm->lpstk_ptr - 1];
}

static struct loop *
lpstk_pop(tbvm *vm, int var, struct loop *l_store, bool pop_match)
{
	int slot;

	for (slot = vm->lpstk_ptr - 1; slot >= 0; slot--) {
		if (vm->lpstk[slot].var == var) {
			*l_store = vm->lpstk[slot];
			vm->lpstk_ptr = pop_match ? slot : slot + 1;
			return l_store;
		}
	}
	return NULL;
}

/*********** Variable routines **********/

static void
var_init(tbvm *vm)
{
	memset(vm->vars, 0, sizeof(vm->vars));
}

static struct value *
var_slot(tbvm *vm, int idx)
{
	if (idx < 0 || idx >= NUM_VARS) {
		vm_abort(vm, "!INVALID VARIABLE INDEX");
	}
	return &vm->vars[idx];
}

static int
var_get_integer(tbvm *vm, int idx)
{
	struct value *slot = var_slot(vm, idx);
	return slot->integer;
}

static int
var_set_integer(tbvm *vm, int idx, int val)
{
	struct value *slot = var_slot(vm, idx);
	return (slot->integer = val);
}

/*********** Default I/O routines **********/

static int
default_getchar(void *v)
{
	return getchar();
}

static void
default_putchar(void *v, int c)
{
	(void) putchar(c);
}

/*********** Program execution helper routines **********/

static void
skip_whitespace(tbvm *vm)
{
	char c;

	for (;;) {
		c = vm->lbuf[vm->lbuf_ptr];
		if (c == ' ' || c == '\t') {
			vm->lbuf_ptr++;
		} else {
			return;
		}
	}
}

static void
progstore_init(tbvm *vm)
{
	int i;

	for (i = 0; i < MAX_LINENO; i++) {
		if (vm->progstore[i] != NULL) {
			free(vm->progstore[i]);
			vm->progstore[i] = NULL;
		}
	}
	vm->first_line = vm->last_line = 0;
}

static char *
find_line(tbvm *vm, int lineno)
{
	if (lineno < 1 || lineno > MAX_LINENO) {
		return NULL;
	}

	return vm->progstore[lineno - 1];
}

static void
update_bookends(tbvm *vm, int lineno, char *cp)
{
	if (cp != NULL) {
		if (vm->first_line < 1 || vm->first_line > lineno) {
			vm->first_line = lineno;
		}
		if (vm->last_line < 1 || vm->last_line < lineno) {
			vm->last_line = lineno;
		}
		assert(vm->first_line > 0);
		assert(vm->last_line > 0);
		return;
	}

	if (lineno == vm->first_line) {
		vm->first_line = 0;
		for (int i = lineno; i <= MAX_LINENO; i++) {
			if (vm->progstore[i - 1] != NULL) {
				vm->first_line = i;
				break;
			}
		}
		if (vm->first_line == 0) {
			vm->last_line = 0;
			return;
		}
	}

	if (lineno == vm->last_line) {
		vm->last_line = 0;
		for (int i = lineno; i >= 1; i--) {
			if (vm->progstore[i - 1] != NULL) {
				vm->last_line = i;
				break;
			}
		}
		assert(vm->last_line != 0);
	}
}

static void
insert_line(tbvm *vm, int lineno)
{
	int i = lineno - 1;
	char *cp;
	size_t len;

	assert(lineno >= 1 && lineno <= MAX_LINENO);
	assert(vm->lbuf == vm->direct_lbuf);

	skip_whitespace(vm);
	for (cp = &vm->lbuf[vm->lbuf_ptr]; *cp != END_OF_LINE; cp++) {
		/* skip to end of line */
	}
	len = cp - &vm->lbuf[vm->lbuf_ptr];
	if (len == 0) {
		cp = NULL;		/* delete line */
	} else {
		cp = malloc(len + 1);	/* include end-of-line */
		memcpy(cp, &vm->lbuf[vm->lbuf_ptr], len + 1);
	}

	if (vm->progstore[i] != NULL) {
		free(vm->progstore[i]);
	}
	vm->progstore[i] = cp;
	update_bookends(vm, lineno, cp);
}

static void
list_program(tbvm *vm)
{
	int i, width;
	char *cp;

	if (vm->first_line == 0) {
		assert(vm->last_line == 0);
		return;
	} else {
		assert(vm->last_line >= vm->first_line);
	}

	width = printed_number_width(vm->last_line);
	for (i = vm->first_line - 1; i < vm->last_line; i++) {
		if (vm->progstore[i] == NULL) {
			continue;
		}
		print_number_justified(vm, i + 1, width);
		(*vm->io_putchar)(vm->context, ' ');
		for (cp = vm->progstore[i]; *cp != END_OF_LINE; cp++) {
			(*vm->io_putchar)(vm->context, *cp);
		}
		print_crlf(vm);
	}
}

static int
next_line(tbvm *vm)
{
	int i, rv = -1;

	if (vm->lineno == 0) {
		rv = vm->first_line;
	} else if (vm->last_line > 0) {
		for (i = vm->lineno + 1; i <= vm->last_line; i++) {
			if (find_line(vm, i) != NULL) {
				rv = i;
				break;
			}
		}
	}

	if (rv == 0) {
		rv = -1;
	}
	return rv;
}

static void
init_vm(tbvm *vm)
{
	progstore_init(vm);
	var_init(vm);

	vm->cstk_ptr = 0;
	vm->sbrstk_ptr = 0;
	vm->aestk_ptr = 0;

	vm->lbuf = vm->direct_lbuf;
	vm->lbuf_ptr = 0;
	vm->lineno = 0;

	vm->direct = true;
}

static bool
check_break(tbvm *vm)
{
	if (vm->break_received) {
		print_crlf(vm);
		print_cstring(vm, "BREAK");
		print_crlf(vm);
		direct_mode(vm, 0);
		vm->break_received = 0;
		return true;
	}
	return false;
}

static void
set_line_ext(tbvm *vm, int lineno, int ptr, bool fatal, bool restoring)
{
	char *lbuf;

	if (lineno == 0) {
		/* XFER will error this for GOTO / GOSUB. */
		direct_mode(vm, ptr);
		return;
	}

	if (lineno < 0 || lineno > MAX_LINENO) {
		if (fatal) {
			vm_abort(vm, "!LINE NUMBER OUT OF RANGE");
		} else {
			basic_line_number_error(vm);
		}
	}

	if (ptr < 0 || ptr >= SIZE_LBUF) {
		vm_abort(vm, "!LBUF POINTER OUT OF RANGE");
	}

	lbuf = find_line(vm, lineno);
	if (lbuf == NULL) {
		if (fatal) {
			vm_abort(vm, "!MISSING LINE");
		} else {
			basic_missing_line_error(vm);
		}
	}

	vm->lbuf = lbuf;
	vm->lbuf_ptr = ptr;
	vm->lineno = lineno;
	if (!restoring) {
		vm->pc = vm->executor_pc;
	}
}

static void
set_line(tbvm *vm, int lineno, int ptr, bool fatal)
{
	set_line_ext(vm, lineno, ptr, fatal, false);
}

static void
restore_line(tbvm *vm, int lineno, int ptr)
{
	set_line_ext(vm, lineno, ptr, true, true);
}

static void
next_statement(tbvm *vm)
{
	int line = next_line(vm);

	if (vm->direct || line == -1) {
		direct_mode(vm, 0);
	} else {
		set_line(vm, line, 0, true);
	}
}

static char
get_progbyte(tbvm *vm)
{
	if (vm->pc < 0 || vm->pc >= vm->vm_progsize) {
		vm_abort(vm, "!VM PROGRAM COUNTER OUT OF RANGE");
	}
	return vm->vm_prog[vm->pc++];
}

static unsigned char
get_opcode(tbvm *vm)
{
	vm->opc_pc = vm->pc;
	return (unsigned char)get_progbyte(vm);
}

static int
get_label(tbvm *vm)
{
	int tmp;

	tmp  = (unsigned char)get_progbyte(vm);
	tmp |= (unsigned char)get_progbyte(vm) << 8;

	return tmp;
}

static int
get_literal(tbvm *vm)
{
	return get_progbyte(vm);
}

static void
advance_cursor(tbvm *vm, int count)
{
	vm->lbuf_ptr += count;
}

static char
get_linebyte(tbvm *vm)
{
	return vm->lbuf[vm->lbuf_ptr++];
}

static char
peek_linebyte(tbvm *vm, int idx)
{
	return vm->lbuf[vm->lbuf_ptr + idx];
}

/*********** Opcode implementations **********/

typedef void (*opc_impl_func_t)(tbvm *);

#define	IMPL(x)	static void OPC_ ## x ## _impl(tbvm *vm)

/*
 * Delete leading blanks.  If string matches the BASIC line, advance
 * cursor over the matched string and execute the next IL instruction.
 * If a match fails, execute the IL instruction at the label lbl.
 */
IMPL(TST)
{
	int label = get_label(vm);
	int count;
	char line_c, prog_c;

	skip_whitespace(vm);

	for (count = 0;;) {
		prog_c = get_progbyte(vm);
		line_c = peek_linebyte(vm, count);
		if ((prog_c & 0x7f) != line_c) {
			vm->pc = label;
			return;
		}
		count++;
		if (prog_c & 0x80) {
			break;
		}
	}
	advance_cursor(vm, count);
}

/*
 * Execute the IL subroutine starting at "lbl".
 * Save the IL address following the CALL on the
 * control stack.
 */
IMPL(CALL)
{
	int tmp = get_label(vm);
	cstk_push(vm, vm->pc);
	vm->pc = tmp;
}

/*
 * Return to the IL location specified by the top
 * of the control stack.
 */
IMPL(RTN)
{
	vm->pc = cstk_pop(vm);
}

/*
 * Report a syntax error if after deletion leading blanks the
 * cursor is not positioned toward a carriage return.
 *
 * JTTB: This and NXT are the VM operations to change in order to support
 * multiple statements per line using the ':' separator a'la MS BASIC.
 */
IMPL(DONE)
{
	int c;

	skip_whitespace(vm);
	c = peek_linebyte(vm, 0);

	if (c != END_OF_LINE) {
		basic_syntax_error(vm);
	}
}

/*
 * Continue execution of IL at the address specified.
 */
IMPL(JMP)
{
	vm->pc = get_label(vm);
}

/*
 * Print characters from the BASIC text up to but
 * not including the closing quote mark. If a cr
 * is found in the program text, report an error.
 * Move the cursor to the point following the closing
 * quote.
 *
 * JTTB: While PRN has been generalized in this dialect of Tiny BASIC,
 * PRS is retained because it represents an optimization - there is no
 * need to create (and subsequently dispose of) a string object for the
 * (extremely) common case of printing of an immediate string.
 */
IMPL(PRS)
{
	char c;

	for (;;) {
		c = get_linebyte(vm);
		if (c == DQUOTE) {
			break;
		}
		if (c == END_OF_LINE) {
			basic_syntax_error(vm);
			break;
		}
		(*vm->io_putchar)(vm->context, c);
	}
}

/*
 * Print value obtained by popping the top of the
 * expression stack.
 *
 * JTTB: This differs from the original Tiny BASIC "PRN".
 * The original "PRN" only printed numbers.  This has been
 * generalized to print any "value".  However, it does
 * remain compatible with its use in the original Tiny BASIC
 * VM program.
 */
IMPL(PRN)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);

	switch (value.type) {
	case VALUE_TYPE_INTEGER:
		print_number(vm, value.integer);
		break;

	default:
		vm_abort(vm, "!NO PRINTER FOR VALUE");
	}
}

/*
 * Insert spaces, to move the print head to next zone.
 */
IMPL(SPC)
{
}

/*
 * Output CRLF to Printer.
 */
IMPL(NLINE)
{
	print_crlf(vm);
}

/*
 * If the present mode is direct (line number zero), then return to line
 * collection. Otherwise, select the next line and begin interpretation.
 */
IMPL(NXT)
{
	next_statement(vm);
}

/*
 * Test value at the top of the AE stack to be within range. If not,
 * report an error. If so, attempt to position cursor at that line.
 * If it exists, begin interpretation there; if not report an error.
 */
IMPL(XFER)
{
	int lineno = aestk_pop_integer(vm);

	/* Don't let this put us in direct mode. */
	if (lineno == 0) {
		basic_line_number_error(vm);
	}
	set_line(vm, lineno, 0, false);
}

/*
 * Push present line number on SBRSTK. Report overflow as error.
 */
IMPL(SAV)
{
	/* ensure we return to DIRECT mode if needed */
	int lineno = vm->direct ? 0 : vm->lineno;

	sbrstk_push(vm, lineno, vm->lbuf_ptr);
}

/*
 * Replace current line number with value on SBRSTK.
 * If stack is empty, report error.
 */
IMPL(RSTR)
{
	int lineno, ptr;

	sbrstk_pop(vm, &lineno, &ptr);
	restore_line(vm, lineno, ptr);
}

bool
compare(tbvm *vm)
{
	int val2 = aestk_pop_integer(vm);
	int rel  = aestk_pop_integer(vm);
	int val1 = aestk_pop_integer(vm);
	bool result = false;

	/*
	 * Relation values:
	 *
	 * 0	=
	 * 1	<
	 * 2	<=
	 * 3	<>
	 * 4	>
	 * 5	>=
	 */
	switch (rel) {
	case 0:
		result = val1 == val2;
		break;
	
	case 1:
		result = val1 < val2;
		break;
	
	case 2:
		result = val1 <= val2;
		break;
	
	case 3:
		result = val1 != val2;
		break;
	
	case 4:
		result = val1 > val2;
		break;
	
	case 5:
		result = val1 >= val2;
		break;
	
	default:
		vm_abort(vm, "!INVALID RELATIONAL OPERATOR");
	}

	return result;
}

/*
 * Compare AESTK(SP), the top of the stack, with AESTK(SP-2)
 * as per the relations indicated by AESTK(SP-1). Delete all
 * from stack.  If the condition specified did not match, then
 * perform NXT action.
 */
IMPL(CMPR)
{
	if (! compare(vm)) {
		next_statement(vm);
	}
}

/*
 * This is like CMPR, but on no match, CMPRX branches to a VM label
 * rather than performing NXT.
 */
IMPL(CMPRX)
{
	int label = get_label(vm);

	if (! compare(vm)) {
		vm->pc = label;
	}
}

/*
 * Push the number num onto the AESTK.
 */
IMPL(LIT)
{
	aestk_push_integer(vm, get_literal(vm));
}

/*
 * Read a number from the terminal and push its value onto the AESTK.
 */
IMPL(INNUM)
{
}

/*
 * Return to the line collect routine.
 */
IMPL(FIN)
{
	direct_mode(vm, 0);
}

/*
 * Report syntax error am return to line collect routine.
 */
IMPL(ERR)
{
	basic_syntax_error(vm);
}

/*
 * Replace top two elements of AESTK by their sum.
 */
IMPL(ADD)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);
	aestk_push_integer(vm, num1 + num2);
}

/*
 * Replace top two elements of AESTK by their
 * difference.
 */
IMPL(SUB)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);
	aestk_push_integer(vm, num1 - num2);
}

/*
 * Replace top of AESTK with its negative.
 */
IMPL(NEG)
{
	aestk_push_integer(vm, -aestk_pop_integer(vm));
}

/*
 * Replace top two elements of AESTK by their product.
 */
IMPL(MUL)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);
	aestk_push_integer(vm, num1 * num2);
}

/*
 * Replace top two elements of AESTK by their exponentiation.
 */
IMPL(EXP)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);
	int i, val;

	if (num2 < 0) {
		if (num1 == 0) {
			basic_division_by_zero_error(vm);
		}
		num2 = -num2;
		for (val = 1, i = 0; i < num2; i++) {
			val /= num1;
		}
	} else {
		for (val = 1, i = 0; i < num2; i++) {
			val *= num1;
		}
	}
	aestk_push_integer(vm, val);
}

/*
 * Replace top two elements of AESTK by their quotient.
 */
IMPL(DIV)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);

	if (num2 == 0) {
		basic_division_by_zero_error(vm);
	}
	aestk_push_integer(vm, num1 / num2);
}

/*
 * Replaces top two elements of AESTK by their modulus.
 */
IMPL(MOD)
{
	int num2 = aestk_pop_integer(vm);
	int num1 = aestk_pop_integer(vm);

	if (num2 == 0) {
		basic_division_by_zero_error(vm);
	}
	aestk_push_integer(vm, num1 % num2);
}

/*
 * Place the value at the top of the AESTK into the variable
 * designated by the index specified by the value immediately
 * below it. Delete both from the stack.
 */
IMPL(STORE)
{
	int val = aestk_pop_integer(vm);
	int var = aestk_pop_varref(vm);
	var_set_integer(vm, var, val);
}

/*
 * Test for variable (i.e letter) if present. Place its index value
 * onto the AESTK and continue execution at next suggested location.
 * Otherwise continue at lbl.
 */
IMPL(TSTV)
{
	int label = get_label(vm);
	char c;

	skip_whitespace(vm);
	c = peek_linebyte(vm, 0);
	if (c >= 'A' && c <= 'Z') {
		advance_cursor(vm, 1);
		aestk_push_varref(vm, c - 'A');
	} else {
		vm->pc = label;
	}
}

static bool
parse_number(tbvm *vm, bool advance, int *valp)
{
	int count, val = 0;
	char c;

	skip_whitespace(vm);
	for (count = 0;;) {
		c = peek_linebyte(vm, count);
		if (c < '0' || c > '9') {
			break;
		}
		count++;
		val = (val * 10) + (c - '0');
	}
	if (count) {
		if (advance) {
			advance_cursor(vm, count);
		}
		*valp = val;
		return true;
	}
	return false;
}

/*
 * Test for number. If present, place its value onto the AESTK and
 * continue execution at next suggested location. Otherwise continue at
 * lbl.
 */
IMPL(TSTN)
{
	int label = get_label(vm);
	int val;

	if (parse_number(vm, true, &val)) {
		aestk_push_integer(vm, val);
	} else {
		vm->pc = label;
	}
}

/*
 * Replace top of stack by variable value it indexes.
 */
IMPL(IND)
{
	int var = aestk_pop_varref(vm);
	aestk_push_integer(vm, var_get_integer(vm, var));
}

/*
 * List the contents of the program area.
 */
IMPL(LST)
{
	list_program(vm);
}

/*
 * Perform global initilization.
 * Clears program area, empties GOSUB stack, etc.
 */
IMPL(INIT)
{
	init_vm(vm);
}

/*
 * Input a line to LBUF.
 */
IMPL(GETLINE)
{
	int ch;
	bool quoted = false;

	vm->lbuf = vm->direct_lbuf;
	vm->lbuf_ptr = 0;

	if (! vm->suppress_prompt) {
		print_cstring(vm, "OK");
		print_crlf(vm);
	}
	vm->suppress_prompt = false;

	for (;;) {
		if (check_break(vm)) {
			vm->lbuf_ptr = 0;
		}
		ch = (*vm->io_getchar)(vm->context);
		if (ch == EOF) {
			print_crlf(vm);
			print_cstring(vm, "INPUT DISCONNECTED. GOODBYE.");
			print_crlf(vm);
			vm->vm_run = false;
			return;
		}
		if (ch == END_OF_LINE) {
			vm->lbuf[vm->lbuf_ptr] = (char)ch;
			vm->lbuf_ptr = 0;
			return;
		}
		if (vm->lbuf_ptr == SIZE_LBUF - 1) {
			print_crlf(vm);
			print_cstring(vm, "INPUT LINE TOO LONG.");
			print_crlf(vm);
			vm->lbuf_ptr = 0;
			continue;
		}
		if (ch == DQUOTE) {
			quoted ^= true;
		} else if (!quoted && ch >= 'a' && ch <= 'z') {
			ch = 'A' + (ch - 'a');
		}
		vm->lbuf[vm->lbuf_ptr++] = (char)ch;
	}
}

/*
 * After editing leading blanks, look for a line number. Report error
 * if invalid; transfer to lbl if not present.
 */
IMPL(TSTL)
{
	int label = get_label(vm);
	int val;

	if (parse_number(vm, false, &val)) {
		if (val < 1 || val > MAX_LINENO) {
			basic_line_number_error(vm);
		}
	} else {
		vm->pc = label;
	}
}

/*
 * Insert line after deleting any line with same line number.
 */
IMPL(INSRT)
{
	int val;

	if (!parse_number(vm, true, &val) ||
	    val < 1 || val > MAX_LINENO) {
		basic_line_number_error(vm);
	}
	insert_line(vm, val);

	/*
	 * Suppress the BASIC prompt after inserting a BASIC line
	 * into the program store.
	 */
	vm->suppress_prompt = true;
}

/*
 * Perform initialization for each statement execution. Empties AEXP stack.
 */
IMPL(XINIT)
{
	vm->aestk_ptr = 0;
}

/*
 * Run the stored program.
 */
IMPL(RUN)
{
	vm->direct = false;
	vm->lineno = 0;
	next_statement(vm);
}

/*
 * Exit the VM execution loop.
 */
IMPL(EXIT)
{
	vm->vm_run = false;
}

/*
 * Push a FOR loop onto the loop stack.  The top of the AESTK
 * contains the ending value and the next entry on AESTK contains
 * the starting value.  Ascending or descending is inferred by the
 * starting and ending values.  The final value on the AESTK is
 * the var index.
 *
 * N.B. the loop body will ALWAYS execute at least once, as the
 * terminating condition is checked at the NEXT statement.
 */
IMPL(FOR)
{
	struct loop l;

	l.end_val = aestk_pop_integer(vm);
	l.start_val = aestk_pop_integer(vm);
	l.var = aestk_pop_varref(vm);
	l.lineno = next_line(vm);	/* XXX doesn't handle compound lines */
	l.step = l.end_val > l.start_val ? 1 : -1;

	lpstk_push(vm, &l);

	var_set_integer(vm, l.var, l.start_val);
}

/*
 * Adjust the STEP value of the FOR loop at the top of the loop stack.
 * STEP values must be > 0.  As with the FOR insn, the direction of the
 * step is inferred by the starting and ending values.
 */
IMPL(STEP)
{
	struct loop *l = lpstk_peek_top(vm);
	int step = aestk_pop_integer(vm);

	/* Invalid step value. */
	if (step <= 0) {
		basic_step_error(vm);
	}
	l->step = l->end_val > l->start_val ? step : -step;
}

/*
 * Find the inner-most loop associated with the var index on the AESTK
 * and:
 *
 *	- Pop any loops inside off the stack.
 *	- If the terminating condition is not met, set the BASIC
 *	  line back to the start of the loop.
 *	- Otherwise, pop the loop off the stack and continue to
 *	  the next BASIC line.
 */
IMPL(NXTFOR)
{
	int var = aestk_pop_varref(vm);
	struct loop *l, l_store;
	int newval;
	bool done = false;

	l = lpstk_pop(vm, var, &l_store, false);
	if (l == NULL) {
		basic_next_error(vm);
	}
	newval = var_get_integer(vm, var) + l->step;

	if (l->step < 0) {
		if (newval < l->end_val) {
			done = true;
		}
	} else {
		if (newval > l->end_val) {
			done = true;
		}
	}

	if (done) {
		next_statement(vm);
		l = lpstk_pop(vm, var, &l_store, true);
	} else {
		var_set_integer(vm, var, newval);
		set_line(vm, l->lineno, 0, true);
	}
}

/*
 * Take the value at the top of AESTK and replace it with a random
 * number, depending on the argument:
 *
 * ==> If > 1, an integer in the range of 1 ... num, inclusive.
 *
 * ==> If == 0, a floating point number in the range 0 ... 1.
 *     (When floating point numbers are supported, that is.)
 *
 * ==> If 1 -> error
 *
 * This is more-or-less compatible with MS BASIC.
 */
IMPL(RND)
{
	int num = aestk_pop_integer(vm);

	if (num > 1) {
		aestk_push_integer(vm, (rand() / (RAND_MAX / num + 1)) + 1);
	} else {
		basic_number_range_error(vm);
	}
}

/*
 * Take the value at the top of the AESTK and replace it with its
 * absolute value.
 */
IMPL(ABS)
{
	int num = aestk_pop_integer(vm);

	aestk_push_integer(vm, abs(num));
}

/*
 * Skip whitespace and then test if we are at the end-of-line. If not,
 * branch to the label.
 *
 * Does NOT advance past the END-OF-LINE character!
 */
IMPL(TSTEOL)
{
	int label = get_label(vm);

	skip_whitespace(vm);
	if (peek_linebyte(vm, 0) != END_OF_LINE) {
		vm->pc = label;
	}
}

#undef IMPL

#define	OPC(x)	[OPC_ ## x] = OPC_ ## x ## _impl

static opc_impl_func_t opc_impls[OPC___COUNT] = {
	OPC(TST),
	OPC(CALL),
	OPC(RTN),
	OPC(DONE),
	OPC(JMP),
	OPC(PRS),
	OPC(PRN),
	OPC(SPC),
	OPC(NLINE),
	OPC(NXT),
	OPC(XFER),
	OPC(SAV),
	OPC(RSTR),
	OPC(CMPR),
	OPC(LIT),
	OPC(INNUM),
	OPC(FIN),
	OPC(ERR),
	OPC(ADD),
	OPC(SUB),
	OPC(NEG),
	OPC(MUL),
	OPC(DIV),
	OPC(STORE),
	OPC(TSTV),
	OPC(TSTN),
	OPC(IND),
	OPC(LST),
	OPC(INIT),
	OPC(GETLINE),
	OPC(TSTL),
	OPC(INSRT),
	OPC(XINIT),

	/* JTTB additions. */
	OPC(RUN),
	OPC(EXIT),
	OPC(CMPRX),
	OPC(FOR),
	OPC(STEP),
	OPC(NXTFOR),
	OPC(MOD),
	OPC(EXP),
	OPC(RND),
	OPC(ABS),
	OPC(TSTEOL),
};

#undef OPC

/*********** Interface routines **********/

tbvm
*tbvm_alloc(void)
{
	tbvm *vm = calloc(1, sizeof(*vm));

	vm->io_getchar = default_getchar;
	vm->io_putchar = default_putchar;

	return vm;
}

void
tbvm_exec(tbvm *vm, const char *prog, size_t progsize)
{

	vm->vm_prog = prog;
	vm->vm_progsize = progsize;

	/*
	 * Get the two special labels appended to the end of the
	 * VM program:
	 *
	 *	- Line collector routine
	 *	- Statement executor routine
	 */
	vm->pc = progsize - (OPC_LBL_SIZE * 2);
	vm->collector_pc = get_label(vm);
	vm->executor_pc = get_label(vm);

	vm->pc = vm->opc_pc = 0;
	vm->vm_progsize -= (OPC_LBL_SIZE * 2);

	init_vm(vm);

	vm->vm_run = true;

	if (setjmp(vm->vm_abort_env)) {
		vm->vm_run = false;
	}

	if (setjmp(vm->basic_error_env)) {
		/*
		 * Go back to direct mode and jump to the line collection
		 * routine.
		 */
		direct_mode(vm, 0);
	}

	while (vm->vm_run) {
		string_gc(vm);
		check_break(vm);
		vm->opc = (unsigned char)get_opcode(vm);
		if (vm->opc > OPC___LAST || opc_impls[vm->opc] == NULL) {
			vm_abort(vm, "!UNDEFINED VM OPCODE");
		}
		(*opc_impls[vm->opc])(vm);
	}
}

void
tbvm_break(tbvm *vm)
{
	vm->break_received = 1;
}

void
tbvm_free(tbvm *vm)
{
	string_freeall(vm);
	free(vm);
}
