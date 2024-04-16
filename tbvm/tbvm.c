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

#define	NUM_NVARS	26	/* A - Z */
#define	NUM_SVARS	26	/* A$ - Z$ */
#define	NUM_VARS	(NUM_NVARS + NUM_SVARS)
#define	SVAR_BASE	NUM_NVARS
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
	int lineno;
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
	bool		static_strings_valid;

	void		*context;
	int		(*io_getchar)(void *);
	void		(*io_putchar)(void *, int);

	struct value	vars[NUM_VARS];

	char		direct_lbuf[SIZE_LBUF];

	char		*lbuf;
	int		lbuf_ptr;

	int		ondone;
	int		cstk[SIZE_CSTK];
	int		cstk_ptr;

	struct saveloc	sbrstk[SIZE_SBRSTK];
	int		sbrstk_ptr;

	struct value	aestk[SIZE_AESTK];
	int		aestk_ptr;

	struct loop	lpstk[SIZE_LPSTK];
	int		lpstk_ptr;
};

static char empty_string_str[1] = { 0 };
static struct string empty_string = {
	.str = empty_string_str,
	.len = 0,
};

static string *
string_alloc(tbvm *vm, char *str, size_t len, int lineno)
{
	if (len == 0) {
		return &empty_string;
	}

	string *string = malloc(sizeof(*string));
	if (lineno) {
		/*
		 * This is a static string; just directly reference
		 * the program text.
		 */
		string->str = str;
		string->len = len;
		vm->static_strings_valid = true;
	} else {
		string->str = calloc(1, len + 1);
		if (str != NULL) {
			memcpy(string->str, str, len);
		}
	}
	string->len = len;
	string->lineno = lineno;
	string->refs = 1;

	string->next = vm->strings;
	vm->strings = string;

	return string;
}

static string *
string_concatenate(tbvm *vm, string *str1, string *str2)
{
	string *string = string_alloc(vm, NULL, str1->len + str2->len, 0);
	memcpy(string->str, str1->str, str1->len);
	memcpy(&string->str[str1->len], str2->str, str2->len);

	return string;
}

static void
string_invalidate_all_static(tbvm *vm)
{
	if (vm->static_strings_valid) {
		string *string;

		for (string = vm->strings; string != NULL;
		     string = string->next) {
			if (string->lineno) {
				string->str = empty_string_str;
			}
		}
		vm->static_strings_valid = false;
	}
}

static void
string_free(tbvm *vm, string *string)
{
	if (string != &empty_string) {
		if (string->lineno == 0) {
			free(string->str);
		}
		free(string);
	}
}

static string *
string_retain(tbvm *vm, string *string)
{
	if (string != &empty_string) {
		string->refs++;
		assert(string->refs != 0);
	}
	return string;
}

static void
string_release(tbvm *vm, string *string)
{
	if (string != &empty_string) {
		assert(string->refs != 0);
		string->refs--;
		if (string->refs == 0) {
			vm->strings_need_gc = true;
		}
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

static void
print_strbuf(tbvm *vm, const char *str, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		(*vm->io_putchar)(vm->context, str[i]);
	}
}

static void
print_string(tbvm *vm, string *string)
{
	print_strbuf(vm, string->str, string->len);
}

static int
printed_number_width(int num, int base)
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
	for (width = 0; num != 0; num /= base) {
		width++;
	}
	if (negative_p) {
		width++;
	}
	return width;
}

static char *
format_number(int num, int base, int width, char *buf, size_t bufsize)
{
	bool negative_p = num < 0;
	char *cp = &buf[bufsize];
	int digits = 0;

	if (negative_p) {
		num = -num;
	}

	if (base == 10) {
		do {
			*--cp = '0' + (num % 10);
			num /= 10;
			digits++;
		} while (num != 0);
	} else if (base == 16) {
		unsigned int unum = num, n;
		do {
			n = unum & 0xf;
			unum >>= 4;
			if (n <= 9) {
				*--cp = '0' + n;
			} else {
				*--cp = 'A' + (n - 10);
			}
		} while (unum != 0);
	} else {
		abort();
	}

	if (negative_p) {
		*--cp = '-';
		digits++;
	}

	if (width != 0) {
		if (width > bufsize) {
			width = bufsize;
		}
		while (digits < width) {
			*--cp = ' ';
			digits++;
		}
	}

	return cp;
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
	char buf[PRN_BUFSIZE];
	char *cp;

	cp = format_number(num, 10, width, buf, sizeof(buf));
	print_strbuf(vm, cp, &buf[PRN_BUFSIZE] - cp);
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
reset_stacks(tbvm *vm)
{
	vm->ondone = 0;
	vm->cstk_ptr = 0;
	vm->sbrstk_ptr = 0;
	vm->aestk_ptr = 0;
}

static void
direct_mode(tbvm *vm, int ptr)
{
	reset_stacks(vm);

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
aestk_push_string(tbvm *vm, string *string)
{
	/*
	 * N.B. AESTK now owns the caller's reference.  When
	 * a string is popped from AESTK, the reference ownership
	 * is transferred back to the caller.  If the caller
	 * needs an additional reference, it is responsible for
	 * taking it.
	 */
	struct value value = {
		.type = VALUE_TYPE_STRING,
		.string = string,
	};
	aestk_push_value(vm, &value);
}

static string *
aestk_pop_string(tbvm *vm)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_STRING, &value);
	return value.string;
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
	for (int i = SVAR_BASE; i < NUM_VARS; i++) {
		if (vm->vars[i].type == VALUE_TYPE_STRING) {
			string_release(vm, vm->vars[i].string);
		}
	}
	memset(vm->vars, 0, sizeof(vm->vars));
	string_gc(vm);
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
	if (idx >= SVAR_BASE) {
		vm_abort(vm, "!BAD VAR INDEX");
	}
	if (slot->type != VALUE_TYPE_INTEGER) {
		return 0;
	}
	return slot->integer;
}

static int
var_set_integer(tbvm *vm, int idx, int val)
{
	struct value *slot = var_slot(vm, idx);
	if (idx >= SVAR_BASE) {
		vm_abort(vm, "!BAD VAR INDEX");
	}
	slot->type = VALUE_TYPE_INTEGER;
	return (slot->integer = val);
}

static void
var_get_value(tbvm *vm, int idx, struct value *valp)
{
	struct value *slot = var_slot(vm, idx);
	if (idx >= SVAR_BASE && slot->type != VALUE_TYPE_STRING) {
		valp->type = VALUE_TYPE_STRING;
		valp->string = &empty_string;
	} else {
		*valp = *slot;
	}
}

static void
var_set_value(tbvm *vm, int idx, const struct value *valp)
{
	struct value *slot = var_slot(vm, idx);

	if (idx >= SVAR_BASE) {
		if (valp->type != VALUE_TYPE_STRING) {
			basic_wrong_type_error(vm);
		}
	} else if (valp->type != VALUE_TYPE_INTEGER) {
		basic_wrong_type_error(vm);
	}
	*slot = *valp;
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

static bool
whitespace_p(char c)
{
	return c == ' ' || c == '\t';
}

static void
skip_whitespace(tbvm *vm)
{
	for (;;) {
		if (whitespace_p(vm->lbuf[vm->lbuf_ptr])) {
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
	string_invalidate_all_static(vm);
}

static void
list_program(tbvm *vm, int firstline, int lastline)
{
	int i, width;
	char *cp;

	if (vm->first_line == 0) {
		assert(vm->last_line == 0);
		return;
	} else {
		assert(vm->last_line >= vm->first_line);
	}

	if (firstline < vm->first_line) {
		firstline = vm->first_line;
	}

	if (lastline == 0 || lastline > vm->last_line) {
		lastline = vm->last_line;
	}

	if (firstline > lastline) {
		basic_syntax_error(vm);
	}

	width = printed_number_width(lastline, 10);
	for (i = firstline - 1; i < lastline; i++) {
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

	reset_stacks(vm);

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
 * This is a lot like TST, except we scan forward looking for the string
 * to match.  If we encounter an immediate string, we skip over it, and
 * keep scanning after.
 */
IMPL(SCAN)
{
	int label = get_label(vm);
	int count, saved_pc = vm->pc;
	char line_c, prog_c;
	bool matching = false;
	bool dquote = false;

	skip_whitespace(vm);

	for (count = 0, prog_c = get_progbyte(vm);;) {
		line_c = peek_linebyte(vm, count);
		if (line_c == END_OF_LINE) {
			vm->pc = label;
			return;
		}
		count++;
		if (line_c == DQUOTE) {
			dquote ^= true;
		}
		if (dquote) {
			continue;
		}
		if ((prog_c & 0x7f) == line_c) {
			matching = true;
			if (prog_c & 0x80) {
				break;
			}
			prog_c = get_progbyte(vm);
		} else if (matching) {
			vm->pc = saved_pc;
			prog_c = get_progbyte(vm);
			matching = false;
		}
	}
	advance_cursor(vm, count);
}

/*
 * Advance the cursor to the current end-of-line.
 */
IMPL(ADVEOL)
{
	while (vm->lbuf[vm->lbuf_ptr] != END_OF_LINE) {
		vm->lbuf_ptr++;
	}
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

	/*
	 * If an ONDONE hook has been registered we need to do
	 * the following:
	 *
	 * - Push the PC of this DONE opcode onto the control stack.
	 * - Set the PC to the ONDONE hook address.
	 * - Clear the ONDONE hook handler.
	 * - Get the hell out of Dodge.
	 *
	 * When the ONDONE hook is done doing what it needs to do,
	 * it will RTN, which will pop the saved PC off the control
	 * stack and we'll end up right back here, but without the
	 * ONDONE hook set so we can proceed as normal.
	 */
	if (vm->ondone) {
		cstk_push(vm, vm->opc_pc);
		vm->pc = vm->ondone;
		vm->ondone = 0;
		return;
	}

	skip_whitespace(vm);
	c = peek_linebyte(vm, 0);

	if (c != END_OF_LINE) {
		basic_syntax_error(vm);
	}
}

/*
 * Set a hook to be performed on the next DONE insn.
 */
IMPL(ONDONE)
{
	int label = get_label(vm);

	if (vm->ondone != 0) {
		/* XXX Should this be a stack? */
		/* XXX Better error? */
		basic_syntax_error(vm);
	}
	if (label == 0) {
		vm_abort(vm, "!INVALID ONDONE LABEL");
	}
	vm->ondone = label;
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

	case VALUE_TYPE_STRING:
		print_string(vm, value.string);
		string_release(vm, value.string);
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
	/* XXX "Next zone"?  Just one space, for now. */
	(*vm->io_putchar)(vm->context, ' ');
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
 *
 * JTTB: This routine has been extended to also perform
 * string object concatenation.
 */
IMPL(ADD)
{
	struct value val1, val2;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &val2);
	aestk_pop_value(vm, VALUE_TYPE_ANY, &val1);

	if (val1.type != val2.type) {
		basic_wrong_type_error(vm);
	}

	if (val1.type != VALUE_TYPE_STRING) {
		aestk_push_integer(vm, val1.integer + val2.integer);
	} else {
		string *string = string_concatenate(vm,
		    val1.string, val2.string);
		aestk_push_string(vm, string);
		string_release(vm, val1.string);
		string_release(vm, val2.string);
	}
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
	struct value value;
	int var;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	var = aestk_pop_varref(vm);

	var_set_value(vm, var, &value);
}

/*
 * Test for variable (i.e letter) if present. Place its index value
 * onto the AESTK and continue execution at next suggested location.
 * Otherwise continue at lbl.
 */
IMPL(TSTV)
{
	int label = get_label(vm);
	int var;
	char c;

	skip_whitespace(vm);
	c = peek_linebyte(vm, 0);
	if (c >= 'A' && c <= 'Z') {
		advance_cursor(vm, 1);
		var = c - 'A';
		c = peek_linebyte(vm, 0);
		if (c == '$') {
			advance_cursor(vm, 1);
			var += SVAR_BASE;
		}
		aestk_push_varref(vm, var);
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
	struct value value;

	var_get_value(vm, var, &value);

	if (value.type == VALUE_TYPE_ANY) {
		/* Uninitialized variable. */
		if (var >= SVAR_BASE) {
			aestk_push_string(vm, &empty_string);
		} else {
			aestk_push_integer(vm, 0);
		}
		return;
	}
	if (value.type == VALUE_TYPE_STRING) {
		string_retain(vm, value.string);
	}
	aestk_push_value(vm, &value);
}

/*
 * List the contents of the program area.
 */
IMPL(LST)
{
	list_program(vm, 0, 0);
}

/*
 * List the contents of the program area, range specified.
 */
IMPL(LSTX)
{
	int lastline = aestk_pop_integer(vm);
	int firstline = aestk_pop_integer(vm);

	list_program(vm, firstline, lastline);
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

/*
 * Skip whitespace and then test to see if the cursor is pointing
 * at a string.  If so, create a string object and push it onto
 * the AESTK.  If not, branch to the label.
 */
IMPL(TSTS)
{
	int label = get_label(vm);
	int i;
	string *string;

	skip_whitespace(vm);
	if (peek_linebyte(vm, 0) != DQUOTE) {
		vm->pc = label;
		return;
	}

	advance_cursor(vm, 1);		/* advance past DQUOTE */

	/* Find the end of the string. */
	for (i = 0; peek_linebyte(vm, i) != DQUOTE; i++) {
		if (peek_linebyte(vm, i) == END_OF_LINE) {
			basic_syntax_error(vm);
		}
	}

	/* Create the string object and push it onto the stack. */
	string = string_alloc(vm, &vm->lbuf[vm->lbuf_ptr], i, vm->lineno);
	aestk_push_string(vm, string);

	advance_cursor(vm, i + 1);	/* advance past DQUOTE */
}

static void
num_to_string(tbvm *vm, int base)
{
	int num = aestk_pop_integer(vm);
	int width = printed_number_width(num, base);
	string *string = string_alloc(vm, NULL, width, 0);
	char *cp = format_number(num, base, 0, string->str, width);
	assert(cp == string->str);
	aestk_push_string(vm, string);
}

/*
 * Replace the numeric value on the AESTK with a string representation
 * of that number.
 */
IMPL(STR)
{
	num_to_string(vm, 10);
}

/*
 * Replace the numeric value on the AESTK with a hexadecimal string
 * representation of that number.
 */
IMPL(HEX)
{
	num_to_string(vm, 16);
}

/*
 * Replace the string object on the AESTK with a numeric representation.
 */
IMPL(VAL)
{
	string *string = aestk_pop_string(vm);
	char *cp = string->str;
	char *ecp = string->str + string->len;
	int val = 0;
	bool negative_p = false;

	/* skip leading whitespace */
	while (cp < ecp) {
		if (whitespace_p(*cp)) {
			cp++;
		} else {
			break;
		}
	}

	/* handle unary + / - */
	if (cp < ecp) {
		if (*cp == '+') {
			cp++;
		} else if (*cp == '-') {
			negative_p = true;
			cp++;
		}
	}

	/* parse the digits */
	while (cp < ecp) {
		char c = *cp++;
		if (c < '0' || c > '9') {
			break;
		}
		val = (val * 10) + (c - '0');
	}

	if (val != 0 && negative_p) {
		val = -val;
	}

	aestk_push_integer(vm, val);
}

/*
 * Copy the value at the top of AESTK and push the copy.
 */
IMPL(CPY)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	aestk_push_value(vm, &value);
	aestk_push_value(vm, &value);
}

/*
 * Return the length of the string on the AESTK.
 */
IMPL(STRLEN)
{
	string *string = aestk_pop_string(vm);
	aestk_push_integer(vm, string->len);
	string_release(vm, string);
}

/*
 * Pop the string at the top of AESTK and return the ASCII code for
 * the first character.
 */
IMPL(ASC)
{
	string *string = aestk_pop_string(vm);
	int val;

	if (string->len == 0) {
		val = 0;
	} else {
		val = string->str[0];
	}
	aestk_push_integer(vm, val);
	string_release(vm, string);
}

/*
 * Pops the ASCII code from the AESTK and returns it as a character string.
 */
IMPL(CHR)
{
	int code = aestk_pop_integer(vm);
	string *string = string_alloc(vm, NULL, 1, 0);
	string->str[0] = (char)code;
	aestk_push_string(vm, string);
}

/*
 * Pops the number value from the AESTK, converts it to an integer, and
 * pushes the result.
 */
IMPL(INTVAL)
{
	/*
	 * This is extremely easy since we only currently support
	 * integer numbers.
	 */
	int val = aestk_pop_integer(vm);	/* acts as type check */
	aestk_push_integer(vm, val);
}

/*
 * Pops the number value from the AESTK, checks its sign, and pushes
 * a value indicating the sign.
 *
 * - negative -> -1
 * - zero -> 0
 * - positive -> 1
 */
IMPL(SGN)
{
	int val = aestk_pop_integer(vm);

	if (val < 0) {
		val = -1;
	} else if (val > 0) {
		val = 1;
	}
	aestk_push_integer(vm, val);
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
	OPC(TSTS),
	OPC(STR),
	OPC(VAL),
	OPC(HEX),
	OPC(CPY),
	OPC(LSTX),
	OPC(STRLEN),
	OPC(ASC),
	OPC(CHR),
	OPC(INTVAL),
	OPC(SGN),
	OPC(SCAN),
	OPC(ONDONE),
	OPC(ADVEOL),
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
