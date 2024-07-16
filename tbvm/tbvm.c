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
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef TBVM_CONFIG_INTEGER_ONLY
#include <math.h>
#endif /* ! TBVM_CONFIG_INTEGER_ONLY */

#include "tbvm.h"
#include "tbvm_opcodes.h"
#include "tbvm_program.h"

#define	DOES_NOT_RETURN	__attribute__((__noreturn__))

#define	NUM_NVARS	26	/* A - Z */
#define	NUM_SVARS	26	/* A$ - Z$ */
#define	NUM_VARS	(NUM_NVARS + NUM_SVARS)
#define	SVAR_BASE	NUM_NVARS
#define	SIZE_CSTK	64		/* control stack size */
#define	SIZE_SBRSTK	(64+NUM_NVARS)	/* subroutine stack size */
#define	SIZE_AESTK	64		/* expression stack size */
#define	SIZE_LBUF	256

#define	MAX_LINENO	65535	/* arbitrary */

#define	DQUOTE		'"'
#define	COMMA		','
#define	END_OF_LINE	'\n'
#define	TAB		'\t'

#define	CONS_TABSTOP	10

#ifdef TBVM_CONFIG_INTEGER_ONLY
typedef	int		tbvm_number;
#else
typedef	double		tbvm_number;
#endif

typedef struct value	*var_ref;

struct subr {
	var_ref var;
	int lineno;
	int lbuf_ptr;
	tbvm_number start_val;
	tbvm_number end_val;
	tbvm_number step;
};

#define	SUBR_VAR_ANYVAR		((var_ref)-2)
#define	SUBR_VAR_SUBROUTINE	((var_ref)-1)

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
		tbvm_number	number;
		string *	string;
		var_ref		var_ref;
	};
};
#define	VALUE_TYPE_ANY		0
#define	VALUE_TYPE_NUMBER	1	/* number field */
#define	VALUE_TYPE_STRING	2	/* string field */
#define	VALUE_TYPE_VARREF	10	/* var_ref field */

struct array_dim {
	int	nelem;		/* number of elements in this dimension */
	int	idxsize;	/* total index size of this dimension */
};

struct array {
	int ndim;		/* number of dimensions */
	int totelem;		/* total number of elements */
	struct value *elem;	/* the array elements themselves */
	struct array_dim dims[];/* array dimension info */
};

static size_t
array_size(int ndim)
{
	return sizeof(struct array) + sizeof(struct array_dim) * ndim;
}

struct tbvm {
	jmp_buf		vm_abort_env;
	jmp_buf		basic_error_env;

	const char	*vm_prog;
	size_t		vm_progsize;
	bool		vm_run;
	unsigned int	pc;	/* VM program counter */
	unsigned int	opc_pc;	/* VM program counter of current opcode */
	unsigned char	opc;	/* current opcode */
	unsigned long	vm_insns;/* number of insns executed */

	unsigned int	collector_pc;	/* VM address of collector routine */
	unsigned int	executor_pc;	/* VM address of executor routine */

	bool		suppress_prompt;
	bool		direct;		/* true if in DIRECT mode */
	int		lineno;		/* current BASIC line number */
	int		data_lineno;	/* current BASIC DATA line number */
	int		first_line;
	int		last_line;
	char		*progstore[MAX_LINENO];

	string		*strings;
	unsigned int	strings_need_gc;
	bool		static_strings_valid;

	void		*context;
	const struct tbvm_file_io *file_io;
	void		*cons_file;
	void		*prog_file;
	string		*prog_file_name;

	unsigned int	cons_column;

	const struct tbvm_time_io *time_io;
	const struct tbvm_exc_io *exc_io;

	unsigned int	rand_seed;

	struct value	vars[NUM_VARS];
	struct array	*array_vars[NUM_VARS];

	char		direct_lbuf[SIZE_LBUF];
	char		tmp_buf[SIZE_LBUF];

	char		*lbuf;
	int		lbuf_ptr;

	int		saved_lineno;		/* != 0 when in DATA mode */
	int		saved_lbuf_ptr;
	int		data_lbuf_ptr;

	int		ondone;
	int		cstk[SIZE_CSTK];
	int		cstk_ptr;

	struct subr	sbrstk[SIZE_SBRSTK];
	int		sbrstk_ptr;

	struct value	aestk[SIZE_AESTK];
	int		aestk_ptr;
};

/*********** Forward declarations **********/

static void	prog_file_fini(tbvm *);
static void	exit_data_mode(tbvm *);

/*********** Driver interface routines **********/

static inline int
vm_cons_getchar(tbvm *vm)
{
	return (*vm->file_io->io_getchar)(vm->context, vm->cons_file);
}

static inline void
vm_cons_putchar0(tbvm *vm, int ch)
{
	if (ch == END_OF_LINE) {
		vm->cons_column = 0;
	} else {
		vm->cons_column++;
	}
	(*vm->file_io->io_putchar)(vm->context, vm->cons_file, ch);
}

static void
vm_cons_putchar(tbvm *vm, int ch)
{
	if (ch == TAB) {
		do {
			vm_cons_putchar0(vm, ' ');
		} while ((vm->cons_column % CONS_TABSTOP) != 0);
	} else {
		vm_cons_putchar0(vm, ch);
	}
}

static void *
vm_io_openfile(tbvm *vm, const char *fname, const char *acc)
{
	return (*vm->file_io->io_openfile)(vm->context, fname, acc);
}

static void
vm_io_closefile(tbvm *vm, void *file)
{
	(*vm->file_io->io_closefile)(vm->context, file);
}

static bool
vm_io_check_break(tbvm *vm)
{
	return (*vm->file_io->io_check_break)(vm->context, vm->cons_file);
}

static bool
vm_io_gettime(tbvm *vm, unsigned long *timep)
{
	if (vm->time_io != NULL) {
		return (*vm->time_io->io_gettime)(vm->context, timep);
	}
	return false;
}

static int
vm_io_math_exc(tbvm *vm)
{
	if (vm->exc_io != NULL) {
		return (*vm->exc_io->io_math_exc)(vm->context);
	}
	return 0;
}

/*********** String routines **********/

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
		vm->static_strings_valid = true;
	} else {
		string->str = calloc(1, len + 1);
		if (str != NULL) {
			memcpy(string->str, str, len);
		}
	}
	string->len = len;
	string->lineno = lineno;
	string->refs = 0;

	string->next = vm->strings;
	vm->strings = string;
	vm->strings_need_gc++;
	assert(vm->strings_need_gc != 0);

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

static string *
string_terminate(tbvm *vm, string *str1)
{
	/*
	 * Dynamic strings are NUL-terminated already, but static strings
	 * are not.
	 */
	if (str1->lineno == 0) {
		return str1;
	}
	return string_alloc(vm, str1->str, str1->len, 0);
}

static int
string_compare(string *str1, string *str2)
{
	int len = str1->len < str2->len ? str1->len : str2->len;
	int rv;

	rv = memcmp(str1->str, str2->str, len);
	if (rv == 0) {
		if (str1->len < str2->len) {
			rv = -1;
		} else if (str1->len > str2->len) {
			rv = 1;
		}
	}
	return rv;
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
		if (string->refs == 0) {
			assert(vm->strings_need_gc != 0);
			vm->strings_need_gc--;
		}
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
			vm->strings_need_gc++;
			assert(vm->strings_need_gc != 0);
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
		vm->strings_need_gc = 0;
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
	vm->strings_need_gc = 0;
}

/*********** Print formatting and type conversion helper routines **********/

static void
print_crlf(tbvm *vm)
{
	vm_cons_putchar(vm, '\n');
}

static void
print_cstring(tbvm *vm, const char *msg)
{
	const char *cp;

	for (cp = msg; *cp != '\0'; cp++) {
		vm_cons_putchar(vm, *cp);
	}
}

static void
print_strbuf(tbvm *vm, const char *str, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		vm_cons_putchar(vm, str[i]);
	}
}

static void
print_string(tbvm *vm, string *string)
{
	print_strbuf(vm, string->str, string->len);
}

static int
printed_integer_width(int num)
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

static char *
format_integer(int num, int width, char *buf)
{
	if (width) {
		sprintf(buf, "%*d", width, num);
	} else {
		sprintf(buf, "%d", num);
	}
	return buf;
}

static char *
format_number(tbvm_number num, char *buf)
{
#ifdef TBVM_CONFIG_INTEGER_ONLY
	return format_integer(num, 0, buf);
#else
	/*
	 * Just some notes on MS BASIC number formatting:
	 *
	 * A=1.123456789123456789
	 * print A
	 * -> 1.12345679
	 *
	 * print A/10
	 * -> .112345679
	 *
	 * print A/100
	 * -> .0112345679
	 *
	 * print A/1000
	 * -> 1.12345679E-03
	 *
	 * print A*10
	 * -> 11.2345679
	 *
	 * print A*100
	 * -> 112.345679
	 *
	 * print A*1000
	 * -> 1123.45679
	 *
	 * print A*10000
	 * -> 11234.5679
	 *
	 * print A*100000
	 * -> 112345.679
	 *
	 * print A*1000000
	 * -> 1123456.79
	 *
	 * print A*10000000
	 * -> 11234567.9
	 *
	 * print A*100000000
	 * -> 112345679
	 *
	 * print A*1000000000
	 * -> 1.12345679E+09
	 */
	tbvm_number absnum = fabs(num);

	if (absnum > 0.0 && absnum < 0.01) {
		sprintf(buf, "%.8E", num);
	} else {
		sprintf(buf, "%.9G", num);
	}
	return buf;
#endif /* TBVM_CONFIG_INTEGER_ONLY */
}

static void
print_integer(tbvm *vm, int num)
{
	print_cstring(vm, format_integer(num, 0, vm->tmp_buf));
}

static void
print_number(tbvm *vm, tbvm_number num)
{
	print_cstring(vm, format_number(num, vm->tmp_buf));
}

/*********** BASIC / VM error helper routines **********/

static void DOES_NOT_RETURN
vm_abort(tbvm *vm, const char *msg)
{
	print_cstring(vm, msg);
	print_cstring(vm, ", PC=");
	print_integer(vm, vm->opc_pc);
	print_cstring(vm, ", OPC=");
	print_integer(vm, vm->opc);
	print_crlf(vm);
	vm->vm_run = false;
	longjmp(vm->vm_abort_env, 1);
}

static void DOES_NOT_RETURN
basic_error(tbvm *vm, const char *msg)
{
	if (vm->prog_file != NULL) {
		prog_file_fini(vm);
	}
	vm_cons_putchar(vm, '?');
	print_cstring(vm, msg);
	print_cstring(vm, " ERROR");
	if (! vm->direct) {
		print_cstring(vm, " AT LINE ");
		print_integer(vm, vm->lineno);
	}
	print_crlf(vm);
	if (vm->saved_lineno != 0) {
		exit_data_mode(vm);
	}
	longjmp(vm->basic_error_env, 1);
}

static void DOES_NOT_RETURN
basic_syntax_error(tbvm *vm)
{
	basic_error(vm, "SYNTAX");
}

static void DOES_NOT_RETURN
basic_missing_line_error(tbvm *vm)
{
	basic_error(vm, "MISSING LINE");
}

static void DOES_NOT_RETURN
basic_line_number_error(tbvm *vm)
{
	basic_error(vm, "LINE NUMBER OUT OF RANGE");
}

static void DOES_NOT_RETURN
basic_gosub_error(tbvm *vm)
{
	basic_error(vm, "TOO MANY GOSUBS");
}

static void DOES_NOT_RETURN
basic_return_error(tbvm *vm)
{
	basic_error(vm, "RETURN WITHOUT GOSUB");
}

static void DOES_NOT_RETURN
basic_for_error(tbvm *vm)
{
	basic_error(vm, "TOO MANY FOR LOOPS");
}

static void DOES_NOT_RETURN
basic_next_error(tbvm *vm)
{
	basic_error(vm, "NEXT WITHOUT FOR");
}

static void DOES_NOT_RETURN
basic_expression_error(tbvm *vm)
{
	basic_error(vm, "EXPRESSION TOO COMPLEX");
}

#if 0
static void DOES_NOT_RETURN
basic_too_many_lines_error(tbvm *vm)
{
	basic_error(vm, "TOO MANY LINES");
}
#endif

static void DOES_NOT_RETURN
basic_div0_error(tbvm *vm)
{
	basic_error(vm, "DIVISION BY ZERO");
}

#ifndef TBVM_CONFIG_INTEGER_ONLY
static void DOES_NOT_RETURN
basic_math_error(tbvm *vm)
{
	basic_error(vm, "ARITHMETIC EXCEPTION");
}
#endif /* ! TBVM_CONFIG_INTEGER_ONLY */

static void DOES_NOT_RETURN
basic_number_range_error(tbvm *vm)
{
	basic_error(vm, "NUMBER OUT OF RANGE");
}

static void DOES_NOT_RETURN
basic_wrong_type_error(tbvm *vm)
{
	basic_error(vm, "WRONG VALUE TYPE");
}

static void DOES_NOT_RETURN
basic_file_not_found_error(tbvm *vm)
{
	basic_error(vm, "FILE NOT FOUND");
}

static void DOES_NOT_RETURN
basic_wrong_mode_error(tbvm *vm)
{
	basic_error(vm, "WRONG MODE");
}

static void DOES_NOT_RETURN
basic_illegal_quantity_error(tbvm *vm)
{
	basic_error(vm, "ILLEGAL QUANTITY");
}

static void DOES_NOT_RETURN
basic_out_of_data_error(tbvm *vm)
{
	basic_error(vm, "OUT OF DATA");
}

static void DOES_NOT_RETURN
basic_subscript_error(tbvm *vm)
{
	basic_error(vm, "BAD SUBSCRIPT");
}

static void DOES_NOT_RETURN
basic_redim_error(tbvm *vm)
{
	basic_error(vm, "REDIM'D ARRAY");
}

static void DOES_NOT_RETURN
basic_out_of_memory_error(tbvm *vm)
{
	basic_error(vm, "OUT OF MEMORY");
}

/*********** Abstract number math routines **********/

#ifdef TBVM_CONFIG_INTEGER_ONLY
/*
 * Unimplemented math routines for integer-only BASIC get converted
 * into syntax errors.
 */
static inline tbvm_number
tbvm_math_unimpl(tbvm *vm)
{
	basic_syntax_error(vm);
}

#define	tbvm_ceil(vm, num)		(num)
#define	tbvm_floor(vm, num)		(num)

static tbvm_number
tbvm_abs(tbvm *vm, tbvm_number num)
{
	if (num < 0) {
		return -num;
	}
	return num;
}

static tbvm_number
tbvm_pow(tbvm *vm, tbvm_number num1, tbvm_number num2)
{
	tbvm_number i, val;

	if (num2 < 0) {
		if (num1 == 0) {
			basic_div0_error(vm);
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
	return val;
}

static tbvm_number
tbvm_div(tbvm *vm, tbvm_number num1, tbvm_number num2)
{
	if (num2 == 0) {
		basic_div0_error(vm);
	}
	return num1 / num2;
}

static tbvm_number
tbvm_mod(tbvm *vm, tbvm_number num1, tbvm_number num2)
{
	if (num2 == 0) {
		basic_div0_error(vm);
	}
	return num1 % num2;
}

#define	tbvm_atan(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_cos(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_sin(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_tan(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_exp(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_log(vm, num)		tbvm_math_unimpl((vm))
#define	tbvm_sqrt(vm, num)		tbvm_math_unimpl((vm))

#define	tbvm_const_pi(vm)		tbvm_math_unimpl((vm))

static bool
tbvm_strtonum(const char *cp, char **endptr, tbvm_number *valp)
{
	long val;

	*valp = 0;
	errno = 0;
	val = strtol(cp, endptr, 10);
	if (errno == ERANGE || *endptr == cp) {
		return false;
	}
	if (val > INT_MAX || val < INT_MIN) {
		return false;
	}
	*valp = (int)val;
	return true;
}

#define	check_math_error(vm)		/* nothing */

#else /* ! TBVM_CONFIG_INTEGER_ONLY */

#define	tbvm_ceil(vm, num)		ceil(num)
#define	tbvm_floor(vm, num)		floor(num)
#define	tbvm_abs(vm, num)		fabs(num)
#define	tbvm_pow(vm, num1, num2)	pow((num1), (num2))
#define	tbvm_div(vm, num1, num2)	((num1) / (num2))
#define	tbvm_mod(vm, num1, num2)	fmod((num1), (num2))
#define	tbvm_atan(vm, num)		atan(num)
#define	tbvm_cos(vm, num)		cos(num)
#define	tbvm_sin(vm, num)		sin(num)
#define	tbvm_tan(vm, num)		tan(num)
#define	tbvm_exp(vm, num)		exp(num)
#define	tbvm_log(vm, num)		log(num)
#define	tbvm_sqrt(vm, num)		sqrt(num)

#define	tbvm_const_pi(vm)		M_PI

static bool
tbvm_strtonum(const char *cp, char **endptr, tbvm_number *valp)
{
	errno = 0;
	*valp = strtod(cp, endptr);
	if (errno == ERANGE || *endptr == cp) {
		return false;
	}
	return true;
}

static void
check_math_error(tbvm *vm)
{
	int exc = vm_io_math_exc(vm);

	if (exc == 0) {
		return;
	}

	if (exc & TBVM_EXC_DIV0) {
		basic_div0_error(vm);
	} else if (exc & TBVM_EXC_ARITH) {
		basic_math_error(vm);
	}
}

#endif /* TBVM_CONFIG_INTEGER_ONLY */

/*********** Value routines **********/

static int
number_to_int(tbvm *vm, tbvm_number fval)
{
#ifdef TBVM_CONFIG_INTEGER_ONLY
	return (int)fval;
#else
	int ffval = tbvm_floor(vm, fval);

	if (ffval != fval) {
		basic_illegal_quantity_error(vm);
	}
	return (int)ffval;
#endif /* TBVM_CONFIG_INTEGER_ONLY */
}

static inline tbvm_number
int_to_number(tbvm *vm, int val)
{
	return (tbvm_number)val;
}

static inline bool
integer_p(tbvm *vm, tbvm_number val)
{
	return tbvm_floor(vm, val) == val;
}

static bool
value_valid_p(tbvm *vm, const struct value *value)
{
	bool rv = true;

	switch (value->type) {
	case VALUE_TYPE_NUMBER:
	case VALUE_TYPE_VARREF:
		break;

	case VALUE_TYPE_STRING:
		rv = (value->string != NULL);
		break;

	default:
		rv = false;
	}

	return rv;
}

static void
value_retain(tbvm *vm, struct value *value)
{
	switch (value->type) {
	case VALUE_TYPE_STRING:
		string_retain(vm, value->string);
		break;

	default:
		break;
	}
}

static void
value_init(tbvm *vm, var_ref slot, int type)
{
	switch ((slot->type = type)) {
	case VALUE_TYPE_NUMBER:
		slot->number = 0;
		break;

	case VALUE_TYPE_STRING:
		slot->string = &empty_string;
		break;

	default:
		vm_abort(vm, "!INVALID VALUE INIT");
	}
}

static void
value_release_and_init(tbvm *vm, struct value *value, int type)
{
	switch (value->type) {
	case VALUE_TYPE_STRING:
		string_release(vm, value->string);
		break;

	default:
		break;
	}
	if (type != VALUE_TYPE_ANY) {
		/* Reset to the default value. */
		value_init(vm, value, type);
	}
}

static void
value_release(tbvm *vm, struct value *value)
{
	value_release_and_init(vm, value, VALUE_TYPE_ANY);
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
sbrstk_push(tbvm *vm, const struct subr *subrp)
{
	int slot;

	if ((slot = stack_push(&vm->sbrstk_ptr, SIZE_SBRSTK)) == -1) {
		if (subrp->var == SUBR_VAR_SUBROUTINE) {
			basic_gosub_error(vm);
		} else {
			basic_for_error(vm);
		}
	}
	vm->sbrstk[slot] = *subrp;
}

static struct subr *
sbrstk_peek_top(tbvm *vm)
{
	if (vm->sbrstk_ptr == 0) {
		vm_abort(vm, "!SUBRSTK STACK EMPTY");
	}
	return &vm->sbrstk[vm->sbrstk_ptr - 1];
}

static bool
sbrstk_pop(tbvm *vm, var_ref var, struct subr *subrp, bool pop_match)
{
	int slot;

	for (slot = vm->sbrstk_ptr - 1; slot >= 0; slot--) {
		if ((var == SUBR_VAR_ANYVAR &&
		     vm->sbrstk[slot].var != SUBR_VAR_SUBROUTINE) ||
		    (var != SUBR_VAR_ANYVAR && vm->sbrstk[slot].var == var)) {
			*subrp = vm->sbrstk[slot];
			vm->sbrstk_ptr = pop_match ? slot : slot + 1;
			return true;
		}
	}
	if (var == SUBR_VAR_SUBROUTINE) {
		basic_return_error(vm);
	}
	return false;
}

/*********** Arithmetic Expression stack routines **********/

static void
aestk_push_value(tbvm *vm, struct value *valp)
{
	int slot;

	if (! value_valid_p(vm, valp)) {
		vm_abort(vm, "!PUSHING INVALID VALUE");
	}

	if ((slot = stack_push(&vm->aestk_ptr, SIZE_AESTK)) == -1) {
		basic_expression_error(vm);
	}
	value_retain(vm, valp);
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
	value_release(vm, valp);
	if (type != VALUE_TYPE_ANY && type != valp->type) {
		basic_wrong_type_error(vm);
	}
}

static struct value *
aestk_peek(tbvm *vm, int idx)
{
	if (idx >= vm->aestk_ptr) {
		return NULL;
	}
	return &vm->aestk[vm->aestk_ptr - (1 + idx)];
}

static void
aestk_popn(tbvm *vm, int count)
{
	struct value value;
	int i;

	for (i = 0; i < count; i++) {
		aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	}
}

static void
aestk_reset(tbvm *vm)
{
	struct value value;

	while (vm->aestk_ptr != 0) {
		aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	}
}

static void
aestk_push_number(tbvm *vm, tbvm_number val)
{
	struct value value = {
		.type = VALUE_TYPE_NUMBER,
		.number = val,
	};
	aestk_push_value(vm, &value);
}

static tbvm_number
aestk_pop_number(tbvm *vm)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_NUMBER, &value);
	return value.number;
}

static void
aestk_push_string(tbvm *vm, string *string)
{
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
aestk_push_varref(tbvm *vm, var_ref var)
{
	struct value value = {
		.type = VALUE_TYPE_VARREF,
		.var_ref = var,
	};
	aestk_push_value(vm, &value);
}

static var_ref
aestk_pop_varref(tbvm *vm)
{
	struct value value;

	aestk_pop_value(vm, VALUE_TYPE_VARREF, &value);
	return value.var_ref;
}

/*********** Variable routines **********/

static void
var_release_array(tbvm *vm, int vidx)
{
	struct array *array;
	int i;

	if ((array = vm->array_vars[vidx]) != NULL) {
		vm->array_vars[vidx] = NULL;
		for (i = 0; i < array->totelem; i++) {
			value_release(vm, &array->elem[i]);
		}
		free(array->elem);
		free(array);
	}
}

static void
var_init(tbvm *vm)
{
	int i;

	for (i = 0; i < SVAR_BASE; i++) {
		value_release_and_init(vm, &vm->vars[i], VALUE_TYPE_NUMBER);
		var_release_array(vm, i);
	}
	for (; i < NUM_VARS; i++) {
		value_release_and_init(vm, &vm->vars[i], VALUE_TYPE_STRING);
		var_release_array(vm, i);
	}
}

static int
var_raw_index(tbvm *vm, var_ref var, int *typep)
{
	int idx;

	if (var < &vm->vars[0] || var >= &vm->vars[NUM_VARS]) {
		vm_abort(vm, "!BAD VAR ADDRESS");
	}
	idx = var - &vm->vars[0];
	*typep = idx >= SVAR_BASE ? VALUE_TYPE_STRING : VALUE_TYPE_NUMBER;
	return idx;
}

static var_ref
var_make_ref(tbvm *vm, int type, int idx)
{
	switch (type) {
	case VALUE_TYPE_NUMBER:
		if (idx < 0 || idx >= NUM_NVARS) {
			vm_abort(vm, "!INVALID NUMBER VAR INDEX");
		}
		break;

	case VALUE_TYPE_STRING:
		if (idx < 0 || idx >= NUM_SVARS) {
			vm_abort(vm, "!INVALID STRING VAR INDEX");
		}
		idx += SVAR_BASE;
		break;

	default:
		vm_abort(vm, "!INVALID VARIABLE TYPE");
	}
	return &vm->vars[idx];
}

static int
var_type(tbvm *vm, var_ref var)
{
	return var->type;
}

static tbvm_number
var_get_number(tbvm *vm, var_ref var)
{
	if (var->type != VALUE_TYPE_NUMBER) {
		return 0;
	}
	return var->number;
}

static void
var_set_number(tbvm *vm, var_ref var, tbvm_number val)
{
	if (var->type != VALUE_TYPE_NUMBER) {
		basic_wrong_type_error(vm);
	}
	var->number = val;
}

static void
var_get_value(tbvm *vm, var_ref var, struct value *valp)
{
	switch (var->type) {
	case VALUE_TYPE_NUMBER:
	case VALUE_TYPE_STRING:
		break;

	default:
		vm_abort(vm, "!UNINITIALIZED VARIABLE");
	}
	*valp = *var;
}

static void
var_set_value(tbvm *vm, var_ref var, struct value *valp)
{
	if (valp->type != var->type) {
		basic_wrong_type_error(vm);
	}
	value_release(vm, var);
	value_retain(vm, valp);
	*var = *valp;
}

/*********** Default I/O routines **********/

static void *
default_openfile(void *v, const char *fname, const char *mode)
{
	/* Always fails. */
	return NULL;
}

static void
default_closefile(void *v, void *f)
{
	/* Nothing. */
}

static int
default_getchar(void *v, void *f)
{
	/* Just does console. */
	return getchar();
}

static void
default_putchar(void *v, void *f, int c)
{
	/* Just does console. */
	(void) putchar(c);
}

static const struct tbvm_file_io default_file_io = {
	.io_openfile = default_openfile,
	.io_closefile = default_closefile,
	.io_getchar = default_getchar,
	.io_putchar = default_putchar,
};

/*********** Program execution helper routines **********/

static void
reset_stacks(tbvm *vm)
{
	vm->ondone = 0;
	vm->cstk_ptr = 0;
	vm->sbrstk_ptr = 0;
	aestk_reset(vm);
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

static void
prog_file_fini(tbvm *vm)
{
	vm->cons_file = TBVM_FILE_CONSOLE;
	vm_io_closefile(vm, vm->prog_file);
	vm->prog_file = NULL;
	direct_mode(vm, 0);
}

static bool
whitespace_p(char c)
{
	return c == ' ' || c == '\t';
}

static void
skip_whitespace_buf(const char *buf, int *ptrp)
{
	int ptr = *ptrp;

	for (;;) {
		if (whitespace_p(buf[ptr])) {
			ptr++;
		} else {
			*ptrp = ptr;
			return;
		}
	}
}

static void
skip_whitespace(tbvm *vm)
{
	skip_whitespace_buf(vm->lbuf, &vm->lbuf_ptr);
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

	if (vm->prog_file_name != NULL) {
		string_release(vm, vm->prog_file_name);
		vm->prog_file_name = NULL;
	}
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

	width = printed_integer_width(lastline);
	for (i = firstline - 1; i < lastline; i++) {
		if (vm->progstore[i] == NULL) {
			continue;
		}
		print_cstring(vm, format_integer(i + 1, width, vm->tmp_buf));
		vm_cons_putchar(vm, ' ');
		for (cp = vm->progstore[i]; *cp != END_OF_LINE; cp++) {
			vm_cons_putchar(vm, *cp);
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
	vm->cons_file = TBVM_FILE_CONSOLE;

	vm->rand_seed = 1;
}

static bool
check_break(tbvm *vm)
{
	if (vm_io_check_break(vm)) {
		print_crlf(vm);
		print_cstring(vm, "BREAK");
		print_crlf(vm);
		direct_mode(vm, 0);
		return true;
	}
	return false;
}

static bool
check_input_disconnected(tbvm *vm, char ch)
{
	if (ch == EOF) {
		if (vm->cons_file == TBVM_FILE_CONSOLE) {
			print_crlf(vm);
			print_cstring(vm, "INPUT DISCONNECTED. GOODBYE.");
			print_crlf(vm);
			vm->vm_run = false;
		}
		return true;
	}
	return false;
}

static bool
check_input_eol(tbvm *vm, int ch, char *buf, int *ptrp)
{
	if (ch == END_OF_LINE) {
		buf[*ptrp] = (char)ch;
		*ptrp = 0;
		return true;
	}
	return false;
}

static bool
check_input_too_long(tbvm *vm, int *ptrp)
{
	if (*ptrp == SIZE_LBUF - 1) {
		print_crlf(vm);
		print_cstring(vm, "?INPUT LINE TOO LONG");
		print_crlf(vm);
		*ptrp = 0;
		return true;
	}
	return false;
}

static void
input_needs_redo(tbvm *vm)
{
	/* print_crlf(vm); -- not needed -- user has hit return. */
	print_cstring(vm, "?REDO");
	print_crlf(vm);
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

static unsigned char
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
	return get_progbyte(vm);
}

static int
get_label(tbvm *vm)
{
	int tmp;

	tmp  = get_progbyte(vm);
	tmp |= get_progbyte(vm) << 8;

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
 * Like DONE, but we first check to make sure we're in the correct mode.
 * 1 = DIRECT mode, 0 = RUN mode.
 */
IMPL(DONEM)
{
	int mode = get_literal(vm);

	if ((mode == 0 && vm->direct) ||
	    (mode == 1 && !vm->direct)) {
		basic_wrong_mode_error(vm);
	}

	OPC_DONE_impl(vm);
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
		vm_cons_putchar(vm, c);
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
	case VALUE_TYPE_NUMBER:
		print_number(vm, value.number);
		break;

	case VALUE_TYPE_STRING:
		print_string(vm, value.string);
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
	vm_cons_putchar(vm, TAB);
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
 * If there is a next line, then select it and leave the VM PC unchanged.
 * Otherwise, branch to the specified VM label.
 */
IMPL(NXTLN)
{
	int label = get_label(vm);
	int line = next_line(vm);

	if (vm->direct) {
		vm_abort(vm, "!TSTNXT IN DIRECT MODE");
	}
	if (line == -1) {
		vm->pc = label;
	} else {
		set_line_ext(vm, line, 0, true, true);
	}
}

/*
 * Test value at the top of the AE stack to be within range. If not,
 * report an error. If so, attempt to position cursor at that line.
 * If it exists, begin interpretation there; if not report an error.
 */
IMPL(XFER)
{
	int lineno = number_to_int(vm, aestk_pop_number(vm));

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
	struct subr subr = {
		.var = SUBR_VAR_SUBROUTINE,
		.lineno = vm->direct ? 0 : vm->lineno,
	};

	sbrstk_push(vm, &subr);
}

/*
 * Replace current line number with value on SBRSTK.
 * If stack is empty, report error.
 */
IMPL(RSTR)
{
	struct subr subr;

	sbrstk_pop(vm, SUBR_VAR_SUBROUTINE, &subr, true);
	restore_line(vm, subr.lineno, subr.lbuf_ptr);
}

bool
compare(tbvm *vm)
{
	struct value val1, val2;
	int rel;
	bool result = false;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &val2);
	rel = number_to_int(vm, aestk_pop_number(vm));
	aestk_pop_value(vm, VALUE_TYPE_ANY, &val1);

	/*
	 * Only numbers and string, and they must both being
	 * the same.
	 */
	if ((val1.type != VALUE_TYPE_NUMBER &&
	     val1.type != VALUE_TYPE_STRING) ||
	    val1.type != val2.type) {
		basic_wrong_type_error(vm);
	}

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
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) == 0;
		} else {
			result = val1.number == val2.number;
		}
		break;
	
	case 1:
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) < 0;
		} else {
			result = val1.number < val2.number;
		}
		break;
	
	case 2:
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) <= 0;
		} else {
			result = val1.number <= val2.number;
		}
		break;
	
	case 3:
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) != 0;
		} else {
			result = val1.number != val2.number;
		}
		break;
	
	case 4:
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) > 0;
		} else {
			result = val1.number > val2.number;
		}
		break;
	
	case 5:
		if (val1.type == VALUE_TYPE_STRING) {
			result = string_compare(val1.string, val2.string) >= 0;
		} else {
			result = val1.number >= val2.number;
		}
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
	aestk_push_number(vm, int_to_number(vm, get_literal(vm)));
}

/*
 * Discard the value at the top of the AESTK.
 */
IMPL(POP)
{
	struct value value;
	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
}

static bool
get_input_number(tbvm *vm, const char * const startc, bool pm_ok, tbvm_number *valp)
{
	tbvm_number val = 0;
	char *endc;
	int ptr = 0;

	/*
	 * tbvm_strtonum() uses a C library function that skips leading
	 * whitespace, but we want to do it ourselves in order to be more
	 * strict about what we parse.
	 */
	skip_whitespace_buf(startc, &ptr);

	/*
	 * Allow leading unary + or -.  If one is present, make sure
	 * there is a digit immediately following.
	 */
	if (startc[ptr] == '+' || startc[ptr] == '-') {
		if (!pm_ok || startc[ptr + 1] < '0' || startc[ptr + 1] > '9') {
			return false;
		}
	}

	if (! tbvm_strtonum(startc, &endc, &val)) {
		return false;
	}

	/* Advance the input cursor and skip trailing whitespace. */
	ptr += (int)(endc - startc);
	skip_whitespace_buf(startc, &ptr);

	/* Ensure we're pointing at the end of line. */
	if (startc[ptr] != END_OF_LINE) {
		return false;
	}

	*valp = val;
	return true;
}

static bool
get_input_string(tbvm *vm, char *startc, string **stringp)
{
	/*
	 * The MS BASIC string quoting rules for INPUT are a little
	 * wierd.  This is what I have found through experimentation
	 * with TRS-80 Extended Color BASIC:
	 *
	 * ==> All leading whitespace is stripped.
	 *
	 * ==> Trailing whitespace is **preserved**
	 *
	 * ==> If the string starts with a leading DQUOTE, and another
	 *     DQUOTE is encountered, then only whitespace is allowed
	 *     after the second DQUOTE and only the characters inside
	 *     the DQUOTEs are considered.
	 *
	 * ==> If the string does NOT start with a leading DQUOTE, then
	 *     DQUOTEs inside the string **are preserved**.
	 */
	int ptr = 0;
	bool leading_dquote = false;

	skip_whitespace_buf(startc, &ptr);
	if (startc[ptr] == DQUOTE) {
		leading_dquote = true;
		ptr++;
	}
	startc = &startc[ptr];
	ptr = 0;

	for (;; ptr++) {
		if (startc[ptr] == DQUOTE && leading_dquote) {
			int len = ptr++;
			skip_whitespace_buf(startc, &ptr);
			if (startc[ptr] != END_OF_LINE) {
				return false;
			}
			ptr = len;
			break;
		}
		if (startc[ptr] == END_OF_LINE) {
			break;
		}
	}

	*stringp = string_alloc(vm, startc, ptr, 0);
	return true;
}

/*
 * Read a number from the terminal and push its value onto the AESTK.
 */
IMPL(INNUM)
{
	char * const startc = vm->tmp_buf;
	int ch, ptr;
	tbvm_number val;

 get_input:
	print_cstring(vm, "? ");
	for (ptr = 0;;) {
		if (check_break(vm)) {
			direct_mode(vm, 0);
			return;
		}
		ch = vm_cons_getchar(vm);
		if (check_input_disconnected(vm, ch)) {
			return;
		}
		if (check_input_eol(vm, ch, startc, &ptr)) {
			break;
		}
		if (check_input_too_long(vm, &ptr)) {
			continue;
		}
		startc[ptr++] = (char)ch;
	}

	if (! get_input_number(vm, startc, true, &val)) {
		input_needs_redo(vm);
		goto get_input;
	}
	aestk_push_number(vm, val);
}

/*
 * Read a value from the terminal and save it in the specified variable.
 * The accepted type is dictated by the variable type.  The top of the
 * AESTK contains the variable reference, and the next item on the stack
 * is the number of prompt chars to print.  The number of prompt chars
 * is left on the stack.
 */
IMPL(INVAR)
{
	struct value value;
	char * const startc = vm->tmp_buf;
	var_ref var = aestk_pop_varref(vm);
	int pcount = number_to_int(vm, aestk_pop_number(vm));
	int ch, ptr;

 get_input:
	if (pcount) {
		for (int i = 0; i < pcount; i++) {
			vm_cons_putchar(vm, '?');
		}
		vm_cons_putchar(vm, ' ');
	}
	for (ptr = 0;;) {
		if (check_break(vm)) {
			direct_mode(vm, 0);
			return;
		}
		ch = vm_cons_getchar(vm);
		if (check_input_disconnected(vm, ch)) {
			return;
		}
		if (check_input_eol(vm, ch, startc, &ptr)) {
			break;
		}
		if (check_input_too_long(vm, &ptr)) {
			continue;
		}
		startc[ptr++] = (char)ch;
	}

	if (var_type(vm, var) == VALUE_TYPE_STRING) {
		if (! get_input_string(vm, startc, &value.string)) {
			input_needs_redo(vm);
			goto get_input;
		}
		value.type = VALUE_TYPE_STRING;
	} else {
		if (! get_input_number(vm, startc, true, &value.number)) {
			input_needs_redo(vm);
			goto get_input;
		}
		value.type = VALUE_TYPE_NUMBER;
	}
	var_set_value(vm, var, &value);
	aestk_push_number(vm, int_to_number(vm, pcount));
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
 * string object concatenation.  ADD is also used by the
 * VM program not in conjunction with expressions and thus
 * also handles integers.
 */
IMPL(ADD)
{
	struct value val1, val2;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &val2);
	aestk_pop_value(vm, VALUE_TYPE_ANY, &val1);

	/* Both values must be the same type. */
	if (val1.type != val2.type) {
		basic_wrong_type_error(vm);
	}

	switch (val1.type) {
	case VALUE_TYPE_NUMBER:
		aestk_push_number(vm, val1.number + val2.number);
		check_math_error(vm);
		break;

	case VALUE_TYPE_STRING:
		aestk_push_string(vm,
		    string_concatenate(vm, val1.string, val2.string));
		break;

	default:
		basic_wrong_type_error(vm);
	}
}

/*
 * Replace top two elements of AESTK by their
 * difference.
 */
IMPL(SUB)
{
	tbvm_number num2 = aestk_pop_number(vm);
	tbvm_number num1 = aestk_pop_number(vm);
	aestk_push_number(vm, num1 - num2);
	check_math_error(vm);
}

/*
 * Replace top of AESTK with its negative.
 */
IMPL(NEG)
{
	aestk_push_number(vm, -aestk_pop_number(vm));
	check_math_error(vm);
}

/*
 * Replace top two elements of AESTK by their product.
 */
IMPL(MUL)
{
	tbvm_number num2 = aestk_pop_number(vm);
	tbvm_number num1 = aestk_pop_number(vm);
	aestk_push_number(vm, num1 * num2);
	check_math_error(vm);
}

/*
 * Replace top two elements of AESTK by their exponentiation.
 */
IMPL(POW)
{
	tbvm_number num2 = aestk_pop_number(vm);
	tbvm_number num1 = aestk_pop_number(vm);
	aestk_push_number(vm, tbvm_pow(vm, num1, num2));
	check_math_error(vm);
}

/*
 * Replace top two elements of AESTK by their quotient.
 */
IMPL(DIV)
{
	tbvm_number num2 = aestk_pop_number(vm);
	tbvm_number num1 = aestk_pop_number(vm);
	aestk_push_number(vm, tbvm_div(vm, num1, num2));
	check_math_error(vm);
}

/*
 * Replaces top two elements of AESTK by their modulus.
 */
IMPL(MOD)
{
	tbvm_number num2 = aestk_pop_number(vm);
	tbvm_number num1 = aestk_pop_number(vm);
	aestk_push_number(vm, tbvm_mod(vm, num1, num2));
}

/*
 * Place the value at the top of the AESTK into the variable
 * designated by the index specified by the value immediately
 * below it. Delete both from the stack.
 */
IMPL(STORE)
{
	struct value value;
	var_ref var;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	var = aestk_pop_varref(vm);

	var_set_value(vm, var, &value);
}

/*
 * Store the DATA item at the program cursor into the varable
 * referenced on the stack.  The type if the DATA item is inferred
 * from the variable type.
 */
IMPL(DSTORE)
{
	var_ref var = aestk_pop_varref(vm);
	char *cp0, *cp1;
	unsigned dquotes = 0;

	skip_whitespace(vm);

	cp0 = cp1 = &vm->lbuf[vm->lbuf_ptr];

	/* find the separator or the end-of-line. */
	for (;; cp1++) {
		if (*cp1 == DQUOTE) {
			if (dquotes < 2) {
				if (dquotes == 0) {
					/*
					 * Reject quotes that don't start at
					 * the beginning of the DATA item.
					 */
					if (cp1 != cp0) {
						basic_syntax_error(vm);
					}
					/* Advance over starting quote. */
					cp0++;
				}
				dquotes++;
				continue;
			}
			basic_syntax_error(vm);
		}
		if (*cp1 == COMMA) {
			if (dquotes == 1) {
				/* inside quoted string, not a separator */
				continue;
			}
			break;
		}
		if (*cp1 == END_OF_LINE) {
			if (dquotes == 1) {
				/* EOL inside quote is an error. */
				basic_syntax_error(vm);
			}
			break;
		}
	}

	/* total length is how much we advance the cursor. */
	advance_cursor(vm, cp1 - &vm->lbuf[vm->lbuf_ptr]);

	/* trim trailing whitespace */
	while (cp1 != cp0) {
		cp1--;
		if (! whitespace_p(*cp1)) {
			break;
		}
	}

	/*
	 * If we're not pointing at a dquote now, move forward one so that
	 * we get the last non-whitespace character.
	 */
	if (*cp1 != DQUOTE) {
		cp1++;
	}

	/* Now we know the length of the string we care about. */
	string *string = string_alloc(vm, cp0, cp1 - cp0, vm->lineno);

	/* If we're storing into a numeric var, convert to a number. */
	if (var_type(vm, var) == VALUE_TYPE_NUMBER) {
		/* XXX Code dupliacated with VAL(). */
		string = string_terminate(vm, string);
		tbvm_number val;
		char *cp;

		/*
		 * If it's a quoted string, then flag as the
		 * wrong type.
		 */
		if (dquotes) {
			basic_wrong_type_error(vm);
		}

		if (! tbvm_strtonum(string->str, &cp, &val)) {
			basic_illegal_quantity_error(vm);
		}
		if (*cp != '\0') {
			basic_wrong_type_error(vm);
		}
		var_set_number(vm, var, val);
	} else {
		struct value value = {
			.type = VALUE_TYPE_STRING,
			.string = string,
		};
		var_set_value(vm, var, &value);
	}
}

/*
 * Test for variable (i.e letter) if present. Place its index value
 * onto the AESTK and continue execution at next suggested location.
 * Otherwise continue at lbl.
 */
IMPL(TSTV)
{
	int label = get_label(vm);
	int type = VALUE_TYPE_NUMBER;
	int idx;
	char c;

	skip_whitespace(vm);
	c = peek_linebyte(vm, 0);
	if (c >= 'A' && c <= 'Z') {
		advance_cursor(vm, 1);
		idx = c - 'A';
		c = peek_linebyte(vm, 0);
		if (c == '$') {
			advance_cursor(vm, 1);
			type = VALUE_TYPE_STRING;
		}
		aestk_push_varref(vm, var_make_ref(vm, type, idx));
	} else {
		vm->pc = label;
	}
}

static bool
parse_number_common(tbvm *vm)
{
	skip_whitespace(vm);

	/*
	 * Disallow unary + or - characters.  Those are handled by
	 * expressions.
	 */
	if (vm->lbuf[vm->lbuf_ptr] == '+' ||
	    vm->lbuf[vm->lbuf_ptr] == '-') {
		return false;
	}

	return true;
}

static bool
parse_integer(tbvm *vm, bool advance, int *valp)
{
	long val;
	char *cp;

	if (! parse_number_common(vm)) {
		return false;
	}

	val = strtol(&vm->lbuf[vm->lbuf_ptr], &cp, 10);
	if (cp == &vm->lbuf[vm->lbuf_ptr]) {
		return false;
	}
	if (val < INT_MIN || val > INT_MAX) {
		basic_illegal_quantity_error(vm);
	}
	if (advance) {
		advance_cursor(vm, cp - &vm->lbuf[vm->lbuf_ptr]);
	}
	*valp = (int)val;
	return true;
}

static bool
parse_number(tbvm *vm, bool advance, tbvm_number *valp)
{
#ifdef TBVM_CONFIG_INTEGER_ONLY
	return parse_integer(vm, advance, valp);
#else
	tbvm_number val;
	char *cp;

	if (! parse_number_common(vm)) {
		return false;
	}

	errno = 0;
	val = strtod(&vm->lbuf[vm->lbuf_ptr], &cp);
	if (cp == &vm->lbuf[vm->lbuf_ptr]) {
		return false;
	}
	if (errno == ERANGE) {
		basic_illegal_quantity_error(vm);
	}
	if (advance) {
		advance_cursor(vm, cp - &vm->lbuf[vm->lbuf_ptr]);
	}
	*valp = val;
	return true;
#endif /* TBVM_CONFIG_INTEGER_ONLY */
}

/*
 * Test for number. If present, place its value onto the AESTK and
 * continue execution at next suggested location. Otherwise continue at
 * lbl.
 */
IMPL(TSTN)
{
	int label = get_label(vm);
	tbvm_number val;

	if (parse_number(vm, true, &val)) {
		aestk_push_number(vm, val);
	} else {
		vm->pc = label;
	}
}

/*
 * Replace top of stack by variable value it indexes.
 */
IMPL(IND)
{
	struct value value;

	var_get_value(vm, aestk_pop_varref(vm), &value);
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
	int lastline = number_to_int(vm, aestk_pop_number(vm));
	int firstline = number_to_int(vm, aestk_pop_number(vm));

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

	if (! vm->suppress_prompt && vm->prog_file == NULL) {
		print_cstring(vm, "OK");
		print_crlf(vm);
	}
	vm->suppress_prompt = false;

	for (;;) {
		if (check_break(vm)) {
			vm->lbuf_ptr = 0;
		}
		ch = vm_cons_getchar(vm);
		if (check_input_disconnected(vm, ch)) {
			if (vm->cons_file == vm->prog_file) {
				/* Finished loading a program. */
				prog_file_fini(vm);
			}
			return;
		}
		if (check_input_eol(vm, ch, vm->lbuf, &vm->lbuf_ptr)) {
			return;
		}
		if (check_input_too_long(vm, &vm->lbuf_ptr)) {
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

	if (parse_integer(vm, false, &val)) {
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

	if (!parse_integer(vm, true, &val) ||
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

static string *
get_prog_filename(tbvm *vm)
{
	struct value value;
	string *filename = NULL;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);
	switch (value.type) {
	case VALUE_TYPE_NUMBER:
		if (value.number == 0 && vm->prog_file_name != NULL) {
			filename = vm->prog_file_name;
		}
		break;

	case VALUE_TYPE_STRING:
		filename = value.string;
		break;

	default:
		break;
	}

	if (filename != NULL) {
		if (filename != vm->prog_file_name) {
			if (vm->prog_file_name != NULL) {
				string_release(vm, vm->prog_file_name);
			}
			filename = string_terminate(vm, filename);
			string_retain(vm, filename);
			vm->prog_file_name = filename;
		}
	}

	return filename;
}

/*
 * Load a program into the program store.  This is accomplished
 * by opening the program file, setting it as the console file,
 * and then entering the line collector routine.  The GETLINE
 * opcode will then detect the EOL condition from the program
 * file and switch back to the standard console file.
 */
IMPL(LDPRG)
{
	string *filename = get_prog_filename(vm);

	if (filename != NULL) {
		vm->prog_file = vm_io_openfile(vm, filename->str, "I");
	}

	if (vm->prog_file == NULL) {
		basic_file_not_found_error(vm);
	}

	progstore_init(vm);
	var_init(vm);
	reset_stacks(vm);

	/* Preserve loaded file name. */
	string_retain(vm, filename);
	vm->prog_file_name = filename;

	vm->cons_file = vm->prog_file;
	vm->pc = vm->collector_pc;
}

/*
 * Save the program in the program store.  This is accomplished
 * by opening the program file, setting it as the console file,
 * and then listing the program.  Once the program listing is
 * complete, the file is closed and we switch back to the standard
 * console file.
 */
IMPL(SVPRG)
{
	string *filename = get_prog_filename(vm);
	void *file = NULL;

	if (filename != NULL) {
		file = vm_io_openfile(vm, filename->str, "O");
	}

	if (file == NULL) {
		basic_file_not_found_error(vm);
	}

	vm->cons_file = file;
	list_program(vm, 0, 0);
	vm->cons_file = TBVM_FILE_CONSOLE;
	vm_io_closefile(vm, file);
	direct_mode(vm, 0);
}

/*
 * Perform initialization for each statement execution. Empties AEXP stack.
 */
IMPL(XINIT)
{
	/*
	 * A statement while loading a program is no bueno.
	 */
	if (vm->prog_file != NULL) {
		basic_syntax_error(vm);
	}
	aestk_reset(vm);
}

/*
 * Run the stored program.
 */
IMPL(RUN)
{
	var_init(vm);
	reset_stacks(vm);

	vm->direct = false;
	vm->lineno = 0;
	vm->data_lineno = 0;
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
	struct subr subr;

	subr.end_val = aestk_pop_number(vm);
	subr.start_val = aestk_pop_number(vm);
	subr.var = aestk_pop_varref(vm);
	subr.lineno = next_line(vm);	/* XXX doesn't handle compound lines */
	subr.step = 1;

	sbrstk_push(vm, &subr);

	var_set_number(vm, subr.var, subr.start_val);
}

/*
 * Adjust the STEP value of the FOR loop at the top of the loop stack.
 * STEP values must be > 0.  As with the FOR insn, the direction of the
 * step is inferred by the starting and ending values.
 */
IMPL(STEP)
{
	struct subr *subr = sbrstk_peek_top(vm);
	tbvm_number step = aestk_pop_number(vm);

	if (subr->var == SUBR_VAR_SUBROUTINE) {
		vm_abort(vm, "!STEPPING A SUBROUTINE");
	}
	if (step == 0) {
		basic_illegal_quantity_error(vm);
	}

	subr->step = step;
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
	struct value value;
	var_ref var;
	struct subr subr;
	tbvm_number newval;
	bool done = false;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &value);

	if (value.type == VALUE_TYPE_VARREF) {
		var = value.var_ref;
	} else if (value.type == VALUE_TYPE_NUMBER) {
		/*
		 * Perform NEXT for whichever is the inner-most FOR
		 * loop.
		 */
		var = SUBR_VAR_ANYVAR;
	} else {
		vm_abort(vm, "!INVALID NXTFOR");
	}

	if (! sbrstk_pop(vm, var, &subr, false)) {
		basic_next_error(vm);
	}
	if (var == SUBR_VAR_ANYVAR) {
		/* Found the inner-most FOR loop; recover the var. */
		var = subr.var;
	}
	newval = var_get_number(vm, var) + subr.step;

	if (subr.step < 0) {
		if (newval < subr.end_val) {
			done = true;
		}
	} else {
		if (newval > subr.end_val) {
			done = true;
		}
	}
	check_math_error(vm);

	if (done) {
		next_statement(vm);
		sbrstk_pop(vm, var, &subr, true);
	} else {
		var_set_number(vm, var, newval);
		set_line(vm, subr.lineno, 0, true);
	}
}

/*
 * Take the value at the top of AESTK and replace it with a random
 * number, depending on the argument:
 *
 * ==> If > 1, an integer in the range of 1 ... num, inclusive.
 *
 * ==> If == 0, a floating point number in the range 0 ... 1.
 *
 * ==> If 1 -> error
 *
 * This is more-or-less compatible with MS BASIC.
 */
IMPL(RND)
{
	tbvm_number num = aestk_pop_number(vm);

	if (num > 1) {
		unsigned int unum = (unsigned int)tbvm_floor(vm, num);
		aestk_push_number(vm,
		    (tbvm_number)((rand_r(&vm->rand_seed) /
				   (RAND_MAX / unum + 1)) + 1));
	} else if (num == 0) {
		aestk_push_number(vm,
		    ((tbvm_number)rand_r(&vm->rand_seed) /
		     (tbvm_number)RAND_MAX));
	} else {
		basic_number_range_error(vm);
	}
}

/*
 * Seed the random number generator.
 */
IMPL(SRND)
{
	tbvm_number seed = aestk_pop_number(vm);

	if (seed != 0) {
		vm->rand_seed =
		    (unsigned int)tbvm_floor(vm, tbvm_abs(vm, seed));
	} else {
		unsigned long walltime;

		if (! vm_io_gettime(vm, &walltime)) {
			walltime = vm->vm_insns;
		}
		vm->rand_seed = (unsigned int)walltime;
	}
}

/*
 * Take the value at the top of the AESTK and replace it with its
 * absolute value.
 */
IMPL(ABS)
{
	aestk_push_number(vm, tbvm_abs(vm, aestk_pop_number(vm)));
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
 * Test if the cursor is at the start-of-line.  If not, branch to
 * the label.
 */
IMPL(TSTSOL)
{
	int label = get_label(vm);

	if (vm->lbuf_ptr != 0) {
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

/*
 * Replace the numeric value on the AESTK with a string representation
 * of that number.
 */
IMPL(STR)
{
	tbvm_number num = aestk_pop_number(vm);
	char *cp = format_number(num, vm->tmp_buf);

	aestk_push_string(vm, string_alloc(vm, cp, strlen(cp), 0));
}

/*
 * Replace the numeric value on the AESTK with a hexadecimal string
 * representation of that number.
 */
IMPL(HEX)
{
	tbvm_number num = aestk_pop_number(vm);
	char *cp = &vm->tmp_buf[SIZE_LBUF];
	unsigned int unum, n;
	int digits = 0;

	if (num < 0 || !integer_p(vm, num) || num > UINT_MAX) {
		basic_illegal_quantity_error(vm);
	}

	unum = (unsigned int)num;

	*--cp = '\0';
	do {
		n = unum & 0xf;
		unum >>= 4;
		if (n <= 9) {
			*--cp = '0' + n;
		} else {
			*--cp = 'A' + (n - 10);
		}
		digits++;
	} while (unum != 0);

	if (digits & 1) {
		*--cp = '0';
	}

	aestk_push_string(vm, string_alloc(vm, cp, strlen(cp), 0));
}

/*
 * Replace the string object on the AESTK with a numeric representation.
 */
IMPL(VAL)
{
	string *string = string_terminate(vm, aestk_pop_string(vm));
	tbvm_number val;
	char *cp;

	if (! tbvm_strtonum(string->str, &cp, &val)) {
		if (cp == string->str) {
			val = 0;
		} else {
			basic_illegal_quantity_error(vm);
		}
	}
	aestk_push_number(vm, val);
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
	aestk_push_number(vm, (tbvm_number)string->len);
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
	aestk_push_number(vm, (tbvm_number)val);
}

/*
 * Pops the ASCII code from the AESTK and returns it as a character string.
 */
IMPL(CHR)
{
	tbvm_number val = aestk_pop_number(vm);

	if (!integer_p(vm, val) || val < 0 || val > UCHAR_MAX) {
		basic_illegal_quantity_error(vm);
	}
	int code = (int)val;

	string *string = string_alloc(vm, NULL, 1, 0);
	string->str[0] = (unsigned char)code;
	aestk_push_string(vm, string);
}

/*
 * Pops the number value from the AESTK, converts it to an integer, and
 * pushes the result.
 */
IMPL(FIX)
{
	tbvm_number val = aestk_pop_number(vm);	/* acts as type check */

	if (val >= 0) {
		val = tbvm_floor(vm, val);
	} else {
		val = tbvm_ceil(vm, val);
	}
	aestk_push_number(vm, val);
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK and performs the floor() function.
 */
IMPL(FLR)
{
	tbvm_number val = tbvm_floor(vm, aestk_pop_number(vm));
	aestk_push_number(vm, val);
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK and performs the ceil() function.
 */
IMPL(CEIL)
{
	tbvm_number val = tbvm_ceil(vm, aestk_pop_number(vm));
	aestk_push_number(vm, val);
	check_math_error(vm);
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
	tbvm_number val = aestk_pop_number(vm);

	if (val < 0) {
		val = -1;
	} else if (val > 0) {
		val = 1;
	}
	aestk_push_number(vm, val);
}

/*
 * Pops the number value from the AESTK, computes the arc tangent, and
 * pushes the result.
 */
IMPL(ATN)
{
	aestk_push_number(vm, tbvm_atan(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK, computes the cosine, and
 * pushes the result.
 */
IMPL(COS)
{
	aestk_push_number(vm, tbvm_cos(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK, computes the sine, and
 * pushes the result.
 */
IMPL(SIN)
{
	aestk_push_number(vm, tbvm_sin(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK, computes the tangent, and
 * pushes the result.
 */
IMPL(TAN)
{
	aestk_push_number(vm, tbvm_tan(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pop the number from the AESTK and convert degrees to radians (mode 1)
 * or radians to degrees (mode 0).
 */
IMPL(DEGRAD)
{
	int mode = get_literal(vm);
	tbvm_number val = aestk_pop_number(vm);

	if (mode) {
		/* degrees to radians */
		val = val * tbvm_const_pi(vm) / 180;
	} else {
		/* radians to degrees */
		val = val * 180 / tbvm_const_pi(vm);
	}
	aestk_push_number(vm, val);
}

/*
 * Pops the number value from the AESTK, computes the base e exponential, and
 * pushes the result.
 */
IMPL(EXP)
{
	aestk_push_number(vm, tbvm_exp(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK, computes the natural logarithm, and
 * pushes the result.
 */
IMPL(LOG)
{
	aestk_push_number(vm, tbvm_log(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Pops the number value from the AESTK, computes the square root, and
 * pushes the result.
 */
IMPL(SQR)
{
	aestk_push_number(vm, tbvm_sqrt(vm, aestk_pop_number(vm)));
	check_math_error(vm);
}

/*
 * Make a string consisting of the specified number of characters,
 * specified either by ASCII code or taken from the first character
 * of a string.
 */
IMPL(MKS)
{
	struct value val2;
	int count, code;
	char ch;

	aestk_pop_value(vm, VALUE_TYPE_ANY, &val2);
	count = number_to_int(vm, aestk_pop_number(vm));

	if (count < 1 || count > 255) {
		basic_illegal_quantity_error(vm);
	}

	switch (val2.type) {
	case VALUE_TYPE_NUMBER:
		code = number_to_int(vm, val2.number);
		if (code < 0 || code > UCHAR_MAX) {
			basic_illegal_quantity_error(vm);
		}
		ch = (char)code;
		break;

	case VALUE_TYPE_STRING:
		if (val2.string->len < 1) {
			basic_illegal_quantity_error(vm);
		}
		ch = val2.string->str[0];
		break;

	default:
		basic_wrong_type_error(vm);
	}

	string *string = string_alloc(vm, NULL, count, 0);
	for (int i = 0; i < count; i++) {
		string->str[i] = ch;
	}
	aestk_push_string(vm, string);
}

/*
 * Pop mode argument from AESTK.  Mode determines the remaining arguments:
 *
 * Mode 0: pop length, position, and string arguments.  Create substring
 * starting at specified position from beginning of string for length
 * characters.
 *
 * Mode 1: pop position and string arguments.  Create substring starting at
 * specified position from the beginning of the string to the end of the
 * string.
 *
 * Mode 2: pop length and string arguments.  Create a substring of the last
 * length characters of the string.
 */
IMPL(SBSTR)
{
	int mode = number_to_int(vm, aestk_pop_number(vm));
	int pos = -1, len = -1;
	string *string, *newstr;

	switch (mode) {
	case 0:
		len = number_to_int(vm, aestk_pop_number(vm));
		pos = number_to_int(vm, aestk_pop_number(vm));
		string = aestk_pop_string(vm);
		if (pos < 1 || len < 0) {
			basic_illegal_quantity_error(vm);
		}
		/* Starting position is 1-referenced. */
		pos--;
		break;

	case 1:
		pos = number_to_int(vm, aestk_pop_number(vm));
		string = aestk_pop_string(vm);
		if (pos < 1) {
			basic_illegal_quantity_error(vm);
		}
		/* Starting position is 1-referenced. */
		pos--;
		if (pos >= string->len) {
			len = 0;
		} else {
			len = string->len - pos;
		}
		break;

	case 2:
		len = number_to_int(vm, aestk_pop_number(vm));
		string = aestk_pop_string(vm);
		if (len < 0) {
			basic_illegal_quantity_error(vm);
		}
		if (len > string->len) {
			pos = 0;
		} else {
			pos = string->len - len;
		}
		break;

	default:
		vm_abort(vm, "!ILLEGAL SBSTR MODE");
	}

	if (len == 0) {
		aestk_push_string(vm, &empty_string);
		return;
	}

	newstr = string_alloc(vm, &string->str[pos], len, string->lineno);
	aestk_push_string(vm, newstr);
}

static void
exit_data_mode(tbvm *vm)
{
	int lineno = vm->saved_lineno;

	vm->data_lineno = vm->lineno;
	vm->data_lbuf_ptr = vm->lbuf_ptr;

	vm->saved_lineno = 0;

	set_line_ext(vm, lineno, vm->saved_lbuf_ptr, true, true);
}

/*
 * Enter or exit DATA scanning mode.
 */
IMPL(DMODE)
{
	int mode = get_literal(vm);

	switch (mode) {
	case 0:		/* normal exit from DATA mode */
	case 2:		/* out-of-data exit from DATA mode */
		if (vm->saved_lineno == 0) {
			vm_abort(vm, "!INVALID EXIT FROM DATA MODE");
		}
		exit_data_mode(vm);
		if (mode == 2) {
			basic_out_of_data_error(vm);
		}
		break;

	case 1:		/* entry into DATA mode */
		if (vm->saved_lineno != 0) {
			vm_abort(vm, "!NESTED ENTRY INTO DATA MODE");
		}

		if (vm->data_lineno == 0) {
			vm->data_lineno = vm->first_line;
			vm->data_lbuf_ptr = 0;
		}

		vm->saved_lineno = vm->lineno;
		vm->saved_lbuf_ptr = vm->lbuf_ptr;

		set_line_ext(vm, vm->data_lineno, vm->data_lbuf_ptr,
		    true, true);
		break;

	case 3:		/* reset data pointer to beginning */
		if (vm->saved_lineno != 0) {
			vm_abort(vm, "!DATA RESET WHILE IN DATA MODE");
		}
		vm->data_lineno = vm->first_line;
		vm->data_lbuf_ptr = 0;
		break;

	default:
		vm_abort(vm, "!INVALID DMODE");
	}
}

static bool
array_get_dimensions(tbvm *vm, int *ndimp, var_ref *varp)
{
	var_ref var;
	struct value *valp;
	int ndim;

	/*
	 * First we have to determine how many dimensions the resulting
	 * array will have.  We do this by walking up the expression
	 * stack and counting the NUMBER elements until we reach a
	 * non-NUMBER.  When we reach a non-NUMBER, it must be the
	 * variable that's indicates which array is to be dimensioned,
	 * otherwise it's an error.
	 *
	 * N.B. The numbers indicate the maximum index, not the number
	 * of elements at that array level.
	 */
	for (ndim = 0;; ndim++) {
		valp = aestk_peek(vm, ndim);
		if (valp == NULL) {
			return false;
		}
		if (valp->type == VALUE_TYPE_NUMBER) {
			if (number_to_int(vm, valp->number) < 0) {
				basic_illegal_quantity_error(vm);
			}
			continue;
		} else if (valp->type == VALUE_TYPE_VARREF) {
			var = valp->var_ref;
			break;
		} else {
			basic_wrong_type_error(vm);
		}
	}

	/*
	 * This shouldn't happen; the parser should flag a syntax
	 * error first.
	 */
	if (ndim == 0) {
		basic_subscript_error(vm);
	}

	*ndimp = ndim;
	*varp = var;
	return true;
}

static void
alloc_array_elems(tbvm *vm, struct array *array, int totelem, int vtype)
{
	int dim, i, idxsize;

	if (totelem <= 0) {
		/* Integer overflow. */
		goto oom;
	}

	/* Pre-compute the size of each dimension index. */
	for (dim = array->ndim - 1, idxsize = 1; dim >= 0; dim--) {
		array->dims[dim].idxsize = idxsize;
		idxsize *= array->dims[dim].nelem;
	}

	array->totelem = totelem;
	array->elem = calloc(totelem, sizeof(*array->elem));
	for (i = 0; i < totelem; i++) {
		value_release_and_init(vm, &array->elem[i], vtype);
	}
	return;

 oom:
	free(array);
	basic_out_of_memory_error(vm);
}

/*
 * Dimension an array variable.
 */
IMPL(DIM)
{
	var_ref var = NULL;
	struct value *valp;
	int i, dim, ndim, vidx, vtype, totelem;
	struct array *array = NULL;

	if (! array_get_dimensions(vm, &ndim, &var)) {
		goto DIM_abort;
	}

	/*
	 * Because arrays actually exist in a different namespace from
	 * regular vars, we are only using the var_ref on the stack as
	 * a name from which we compute the index into the array store.
	 * Arrays are only allocated once DIM'd (including the MS BASIC-
	 * style implicit DIM-on-first-use)
	 *
	 * N.B. var is guaranteed to be non-NULL here.
	 */
	vidx = var_raw_index(vm, var, &vtype);
	if (vm->array_vars[vidx] != NULL) {
		basic_redim_error(vm);
	}

	array = malloc(array_size(ndim));
	array->ndim = ndim;

	/* Compute the total number of value elements. */
	for (totelem = 1, dim = 0, i = ndim - 1; i >= 0; i--, dim++) {
		valp = aestk_peek(vm, i);
		array->dims[dim].nelem = number_to_int(vm, valp->number) + 1;
		totelem *= array->dims[dim].nelem;
	}
	alloc_array_elems(vm, array, totelem, vtype);
	vm->array_vars[vidx] = array;

	/* Now pop the arguments from the expression stack. */
	aestk_popn(vm, ndim + 1);
	return;

 DIM_abort:
	vm_abort(vm, "!BAD DIMENSION");
}

/*
 * Index an array and push the resulting slot reference onto the
 * expression stack.
 */
IMPL(ARRY)
{
	struct array *array;
	struct value *valp;
	var_ref var;
	int i, dim, ndim, totelem, didx, idx, vidx, vtype;

	if (! array_get_dimensions(vm, &ndim, &var)) {
		goto ARRY_abort;
	}

	vidx = var_raw_index(vm, var, &vtype);
	if ((array = vm->array_vars[vidx]) == NULL) {
		/*
		 * This is the first access of this array.  We'll
		 * replicate classical MS BASIC behavior and implicitly
		 * dimension an 11xN element array here.
		 */
		array = malloc(array_size(ndim));
		array->ndim = ndim;

		/* Compute the total number of value elements. */
		for (totelem = 1, dim = 0; dim < ndim; dim++) {
			array->dims[dim].nelem = 11;
			totelem *= array->dims[dim].nelem;
		}
		alloc_array_elems(vm, array, totelem, vtype);
		vm->array_vars[vidx] = array;
	}

	if (ndim != array->ndim) {
		basic_subscript_error(vm);
	}

	/*
	 * Calculate the offset to the final array element, checking
	 * the dimension limits as we go.
	 */
	for (idx = 0, dim = 0, i = ndim - 1; i >= 0; i--, dim++) {
		valp = aestk_peek(vm, i);
		didx = number_to_int(vm, valp->number);
		if (didx >= array->dims[dim].nelem) {
			basic_subscript_error(vm);
		}
		idx += didx * array->dims[dim].idxsize;
	}
	if (idx >= array->totelem) {
		goto ARRY_abort;
	}

	/*
	 * Pop the arguments off the stack and push the resulting
	 * var_ref.
	 */
	aestk_popn(vm, ndim + 1);
	aestk_push_varref(vm, &array->elem[idx]);
	return;

 ARRY_abort:
	vm_abort(vm, "!BAD ARRAY INDEX");
}

/*
 * Advance the cursor.  There are two modes:
 *
 * 0 - Advance the console cursor the specified number of columns.
 *
 * 1 - Advance the console cursor to the specified column (0-referenced).
 *     If the cursor is already at or beyond the specified column, it is
 *     not moved.
 */
IMPL(ADVCRS)
{
	int mode = get_literal(vm);
	int val = number_to_int(vm, aestk_pop_number(vm));

	if (val < 0) {
		basic_illegal_quantity_error(vm);
	}

	if (mode == 1) {
		val = val > vm->cons_column ? val - vm->cons_column : 0;
	}

	while (val--) {
		vm_cons_putchar(vm, ' ');
	}

	/* Just return an empty string so PRINT doesn't croak. */
	aestk_push_string(vm, string_alloc(vm, NULL, 0, 0));
}

/*
 * Convert a string to all-upper-case or all-lower-case based
 * on mode:
 *
 * 0 - lower-case
 *
 * 1 - upper-case
 */
IMPL(UPRLWR)
{
	int doup = get_literal(vm);
	string *arg = aestk_pop_string(vm);
	string *newstr = string_alloc(vm, arg->str, arg->len, 0);
	int i;

	if (doup) {
		for (i = 0; i < newstr->len; i++) {
			if (islower((unsigned char)newstr->str[i])) {
				newstr->str[i] =
				    toupper((unsigned char)newstr->str[i]);
			}
		}
	} else {
		for (i = 0; i < newstr->len; i++) {
			if (isupper((unsigned char)newstr->str[i])) {
				newstr->str[i] =
				    tolower((unsigned char)newstr->str[i]);
			}
		}
	}
	aestk_push_string(vm, newstr);
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
	OPC(POW),
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
	OPC(FIX),
	OPC(SGN),
	OPC(SCAN),
	OPC(ONDONE),
	OPC(ADVEOL),
	OPC(INVAR),
	OPC(POP),
	OPC(LDPRG),
	OPC(SVPRG),
	OPC(DONEM),
	OPC(SRND),
	OPC(FLR),
	OPC(CEIL),
	OPC(ATN),
	OPC(COS),
	OPC(SIN),
	OPC(TAN),
	OPC(EXP),
	OPC(LOG),
	OPC(SQR),
	OPC(MKS),
	OPC(SBSTR),
	OPC(TSTSOL),
	OPC(NXTLN),
	OPC(DMODE),
	OPC(DSTORE),
	OPC(DIM),
	OPC(ARRY),
	OPC(ADVCRS),
	OPC(DEGRAD),
	OPC(UPRLWR),
};

#undef OPC

/*********** Interface routines **********/

const char tbvm_name_string[] = "Jason's Tiny-ish BASIC";
const char tbvm_version_string[] = "0.5";

const char *
tbvm_name(void)
{
	return tbvm_name_string;
}

const char *
tbvm_version(void)
{
	return tbvm_version_string;
}

tbvm
*tbvm_alloc(void *context)
{
	tbvm *vm = calloc(1, sizeof(*vm));

	vm->context = context;
	vm->file_io = &default_file_io;

	tbvm_set_prog(vm, tbvm_program, sizeof(tbvm_program));

	init_vm(vm);

	return vm;
}

void
tbvm_set_file_io(tbvm *vm, const struct tbvm_file_io *io)
{
	vm->file_io = io;
}

void
tbvm_set_time_io(tbvm *vm, const struct tbvm_time_io *io)
{
	vm->time_io = io;
}

void
tbvm_set_exc_io(tbvm *vm, const struct tbvm_exc_io *io)
{
	vm->exc_io = io;
}

void
tbvm_set_prog(tbvm *vm, const char *prog, size_t progsize)
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
}

static void
tbvm_runprog(tbvm *vm)
{
	if (vm->vm_run && (vm->vm_prog == NULL || vm->vm_progsize == 0)) {
		vm_abort(vm, "!NO VM PROG");
	}

#ifndef TBVM_CONFIG_INTEGER_ONLY
	(void) vm_io_math_exc(vm);	/* clear any pending exceptions */
#endif /* TBVM_CONFIG_INTEGER_ONLY */

	while (vm->vm_run) {
		string_gc(vm);
		check_break(vm);
		vm->opc = (unsigned char)get_opcode(vm);
		if (vm->opc > OPC___LAST || opc_impls[vm->opc] == NULL) {
			vm_abort(vm, "!UNDEFINED VM OPCODE");
		}
		(*opc_impls[vm->opc])(vm);
		vm->vm_insns++;
	}
}

void
tbvm_exec(tbvm *vm)
{
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

	tbvm_runprog(vm);
}

void
tbvm_free(tbvm *vm)
{
	string_freeall(vm);
	free(vm);
}
