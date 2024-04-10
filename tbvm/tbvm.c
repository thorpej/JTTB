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

#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "tbvm.h"
#include "tbvm_opcodes.h"

#define	NUM_VARS	26	/* A - Z */
#define	SIZE_CSTK	64	/* control stack size */
#define	SIZE_SBRSTK	64	/* subroutine stack size */
#define	SIZE_AESTK	64	/* expression stack size */
#define	SIZE_LBUF	256

#define	MAX_LINENO	65535	/* arbitrary */

#define	DQUOTE		'"'
#define	END_OF_LINE	'\n'

struct tbvm {
	jmp_buf		vm_abort_env;
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

	void		*context;
	int		(*io_getchar)(void *);
	void		(*io_putchar)(void *, int);

	int		vars[NUM_VARS];

	char		direct_lbuf[SIZE_LBUF];

	char		*lbuf;
	int		lbuf_ptr;

	int		cstk[SIZE_CSTK];
	int		cstk_ptr;

	struct {
		int lineno;
		int lbuf_ptr;
	}		sbrstk[SIZE_SBRSTK];
	int		sbrstk_ptr;

	int		aestk[SIZE_AESTK];
	int		aestk_ptr;
};

static void
print_crlf(tbvm *vm)
{
	(*vm->io_putchar)(vm->context, '\n');
}

static void
print_string(tbvm *vm, const char *msg)
{
	const char *cp;

	for (cp = msg; *cp != '\0'; cp++) {
		(*vm->io_putchar)(vm->context, *cp);
	}
}

static void
print_number(tbvm *vm, int num)
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

	do {
		*--cp = '0' + (num % 10);
		num /= 10;
	} while (num != 0);

	if (negative_p) {
		*--cp = '-';
	}

	do {
		(*vm->io_putchar)(vm->context, *cp++);
	} while (cp != &buf[PRN_BUFSIZE]);
}

static void
vm_abort(tbvm *vm, const char *msg)
{
	print_string(vm, msg);
	print_string(vm, ", PC=");
	print_number(vm, vm->opc_pc);
	print_string(vm, ", OPC=");
	print_number(vm, vm->opc);
	print_crlf(vm);
	vm->vm_run = false;
	longjmp(vm->vm_abort_env, 1);
}

static void
direct_mode(tbvm *vm)
{
	vm->direct = true;
	vm->pc = vm->collector_pc;
	vm->lineno = 0;
	vm->lbuf = vm->direct_lbuf;
	vm->lbuf_ptr = 0;
}

/*********** BASIC error helper routines **********/

static void
basic_error(tbvm *vm, const char *msg)
{
	print_string(vm, msg);
	if (! vm->direct) {
		print_string(vm, " AT LINE ");
		print_number(vm, vm->lineno);
	}
	print_crlf(vm);

	/*
	 * Go back to direct mode and jump to the line collection
	 * routine.
	 */
	direct_mode(vm);
}

static void
basic_syntax_error(tbvm *vm)
{
	basic_error(vm, "?SYNTAX ERROR");
}

static void
basic_missing_line_error(tbvm *vm)
{
	basic_error(vm, "?MISSING LINE");
}

static void
basic_line_number_error(tbvm *vm)
{
	basic_error(vm, "?LINE NUMBER OUT OF RANGE");
}

static void
basic_gosub_error(tbvm *vm)
{
	basic_error(vm, "?TOO MANY GOSUBS");
}

static void
basic_return_error(tbvm *vm)
{
	basic_error(vm, "?RETURN WITHOUT GOSUB");
}

static void
basic_expression_error(tbvm *vm)
{
	basic_error(vm, "?EXPRESSION TOO COMPLEX");
}

static void
basic_too_many_lines_error(tbvm *vm)
{
	basic_error(vm, "?TOO MANY LINES");
}

static void
basic_division_by_zero_error(tbvm *vm)
{
	basic_error(vm, "?DIVISION BY ZERO");
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
		/* NOTREACHED */
	}
	
	vm->cstk[slot] = val;
}

static int
cstk_pop(tbvm *vm)
{
	int slot;

	if ((slot = stack_pop(&vm->cstk_ptr, SIZE_CSTK)) == -1) {
		vm_abort(vm, "!CONTROL STACK UNDERFLOW");
		/* NOTREACHED */
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
	} else {
		vm->sbrstk[slot].lineno = line;
		vm->sbrstk[slot].lbuf_ptr = ptr;
	}
}

static bool
sbrstk_pop(tbvm *vm, int *linep, int *ptrp)
{
	int slot;

	if ((slot = stack_pop(&vm->sbrstk_ptr, SIZE_SBRSTK)) == -1) {
		basic_return_error(vm);
		return false;
	} else {
		*linep = vm->sbrstk[slot].lineno;
		*ptrp = vm->sbrstk[slot].lbuf_ptr;
		return true;
	}
}

/*********** Arithmetic Expression stack routines **********/

static void
aestk_push(tbvm *vm, int val)
{
	int slot;

	if ((slot = stack_push(&vm->aestk_ptr, SIZE_AESTK)) == -1) {
		basic_expression_error(vm);
	} else {
		vm->aestk[slot] = val;
	}
}

static int
aestk_pop(tbvm *vm)
{
	int slot;

	if ((slot = stack_pop(&vm->aestk_ptr, SIZE_AESTK)) == -1) {
		vm_abort(vm, "!EXPRESSION STACK UNDERFLOW");
		/* NOTREACHED */
	}
	return vm->aestk[slot];
}

/*********** Variable routines **********/

static int *
var_slot(tbvm *vm, int idx)
{
	if (idx < 0 || idx >= NUM_VARS) {
		vm_abort(vm, "!INVALID VARIABLE INDEX");
		/* NOTREACHED */
	}
	return &vm->vars[idx];
}

static int
var_get(tbvm *vm, int idx)
{
	int *slot = var_slot(vm, idx);
	return *slot;
}

static void
var_set(tbvm *vm, int idx, int val)
{
	int *slot = var_slot(vm, idx);
	*slot = val;
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
progstore_init(tbvm *vm)
{
	/* XXX */
}

static char *
find_line(tbvm *vm, int lineno)
{
	/* XXX */
	return NULL;
}

static int
next_line(tbvm *vm)
{
	/* XXX */
	return -1;
}

static void
init_vm(tbvm *vm)
{
	progstore_init(vm);
	memset(vm->vars, 0, sizeof(vm->vars));

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
		print_string(vm, "BREAK");
		print_crlf(vm);
		direct_mode(vm);
		vm->break_received = 0;
		return true;
	}
	return false;
}

static void
set_line(tbvm *vm, int lineno, int ptr, bool fatal)
{
	char *lbuf;

	if (lineno == 0) {
		/* XFER will error this for GOTO / GOSUB. */
		direct_mode(vm);
		return;
	}

	if (lineno < 0 || lineno > MAX_LINENO) {
		if (fatal) {
			vm_abort(vm, "!LINE NUMBER OUT OF RANGE");
			/* NOTREACHED */
		} else {
			basic_line_number_error(vm);
		}
		return;
	}

	if (ptr < 0 || ptr >= SIZE_LBUF) {
		vm_abort(vm, "!LBUF POINTER OUT OF RANGE");
		/* NOTREACHED */
	}

	lbuf = find_line(vm, lineno);
	if (lbuf == NULL) {
		if (fatal) {
			vm_abort(vm, "!MISSING LINE");
			/* NOTREACHED */
		} else {
			basic_missing_line_error(vm);
		}
	}

	vm->lbuf = lbuf;
	vm->lbuf_ptr = ptr;
	vm->lineno = lineno;
	vm->pc = vm->executor_pc;
}

static void
next_statement(tbvm *vm)
{
	int line = next_line(vm);

	if (line == -1) {
		direct_mode(vm);
	} else {
		set_line(vm, line, 0, true);
	}
}

static char
get_progbyte(tbvm *vm)
{
	if (vm->pc < 0 || vm->pc >= vm->vm_progsize) {
		vm_abort(vm, "!VM PROGRAM COUNTER OUT OF RANGE");
		/* NOTREACHED */
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
 * Print number obtained by popping the top of the
 * expression stack.
 */
IMPL(PRN)
{
	print_number(vm, aestk_pop(vm));
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
	int lineno = aestk_pop(vm);

	/* Don't let this put us in direct mode. */
	if (lineno == 0) {
		basic_line_number_error(vm);
	} else {
		set_line(vm, lineno, 0, false);
	}
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

	if (sbrstk_pop(vm, &lineno, &ptr)) {
		set_line(vm, lineno, ptr, true);
	}
}

/*
 * Compare AESTK(SP), the top of the stack, with AESTK(SP-2)
 * as per the relations indicated by AESTK(SP-1). Delete all
 * from stack.  If the condition specified did not match, then
 * perform NXT action.
 */
IMPL(CMPR)
{
	int val2 = aestk_pop(vm);
	int rel  = aestk_pop(vm);
	int val1 = aestk_pop(vm);
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
		/* NOTREACHED */
	}

	if (! result) {
		next_statement(vm);
	}
}

/*
 * Push the number num onto the AESTK.
 */
IMPL(LIT)
{
	aestk_push(vm, get_literal(vm));
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
	direct_mode(vm);
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
	int num2 = aestk_pop(vm);
	int num1 = aestk_pop(vm);
	aestk_push(vm, num1 + num2);
}

/*
 * Replace top two elements of AESTK by their
 * difference.
 */
IMPL(SUB)
{
	int num2 = aestk_pop(vm);
	int num1 = aestk_pop(vm);
	aestk_push(vm, num1 - num2);
}

/*
 * Replace top of AESTK with its negative.
 */
IMPL(NEG)
{
	aestk_push(vm, -aestk_pop(vm));
}

/*
 * Replace top two elements of AESTK by their product.
 */
IMPL(MUL)
{
	int num2 = aestk_pop(vm);
	int num1 = aestk_pop(vm);
	aestk_push(vm, num1 * num2);
}

/*
 * Replace top two elements of AESTK by their quotient.
 */
IMPL(DIV)
{
	int num2 = aestk_pop(vm);
	int num1 = aestk_pop(vm);

	if (num2 == 0) {
		basic_division_by_zero_error(vm);
	} else {
		aestk_push(vm, num1 / num2);
	}
}

/*
 * Place the value at the top of the AESTK into the variable
 * designated by the index specified by the value immediately
 * below it. Delete both from the stack.
 */
IMPL(STORE)
{
	int val = aestk_pop(vm);
	int var = aestk_pop(vm);
	var_set(vm, var, val);
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
		aestk_push(vm, c - 'A');
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
		aestk_push(vm, val);
	} else {
		vm->pc = label;
	}
}

/*
 * Replace top of stack by variable value it indexes.
 */
IMPL(IND)
{
	int var = aestk_pop(vm);
	aestk_push(vm, var_get(vm, var));
}

/*
 * List the contents of the program area.
 */
IMPL(LST)
{
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
		print_string(vm, "OK");
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
			print_string(vm, "INPUT DISCONNECTED. GOODBYE.");
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
			print_string(vm, "INPUT LINE TOO LONG.");
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

	while (vm->vm_run) {
		check_break(vm);
		vm->opc = (unsigned char)get_opcode(vm);
		if (vm->opc > OPC___LAST || opc_impls[vm->opc] == NULL) {
			vm_abort(vm, "!UNDEFINED VM OPCODE");
			/* NOTREACHED */
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
	free(vm);
}
