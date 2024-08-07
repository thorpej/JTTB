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
 * An implementation of Tiny BASIC using a Virtual Machine.
 *
 * http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html
 */

#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#ifndef __vax__
#include <fenv.h>
#pragma STDC FENV_ACCESS ON
#endif /* __vax__ */

#include "../tbvm/tbvm.h"

static tbvm	*vm;

static sig_atomic_t received_sigint;
static sig_atomic_t cnintr_check;
static jmp_buf cnintr_env;

static void
sigint_handler(int sig)
{
	if (cnintr_check) {
		longjmp(cnintr_env, 1);
	}
	received_sigint = 1;
}

_Static_assert(sizeof(sig_atomic_t) >= sizeof(int),
    "sig_atomic_t too small");

static sig_atomic_t fp_exceptions;

static void
sigfpe_action(int sig, siginfo_t *info, void *ctx)
{
	int exc = 0;

	if (info->si_code == FPE_FLTDIV ||
	    info->si_code == FPE_INTDIV) {
		exc |= TBVM_EXC_DIV0;
	} else {
		exc |= TBVM_EXC_ARITH;
	}

	fp_exceptions |= exc;
}

static const char *
mode2stdio(const char *mode)
{
	bool in_p = false;
	bool out_p = false;
	const char *cp;

	for (cp = mode; *cp != '\0'; cp++) {
		switch (*cp) {
		case 'i':
		case 'I':
			in_p = true;
			break;

		case 'o':
		case 'O':
			out_p = true;
			break;

		default:
			break;
		}
	}

	if (in_p && out_p) {
		return "rb+";
	} else if (in_p) {
		return "rb";
	} else if (out_p) {
		return "wb";
	} else {
		return NULL;
	}
}

static void *
jttb_openfile(void *vctx, const char *fname, const char *mode)
{
	mode = mode2stdio(mode);
	if (mode == NULL) {
		return NULL;
	}
	return fopen(fname, mode);
}

static void
jttb_closefile(void *vctx, void *vf)
{
	if (vf != TBVM_FILE_CONSOLE) {
		fclose((FILE *)vf);
	}
}

static int
jttb_getchar(void *vctx, void *vf)
{
	FILE *fp = (vf == TBVM_FILE_CONSOLE) ? stdin : vf;
	int rv;

	if (fp == stdin && setjmp(cnintr_env)) {
		cnintr_check = 0;
		funlockfile(fp);
		return TBVM_BREAK;
	}

 again:
	flockfile(fp);
	cnintr_check = fp == stdin;
	rv = getc_unlocked(fp);
	cnintr_check = 0;
	funlockfile(fp);
	if (rv == EOF) {
		if (ferror(fp)) {
			if (fp == stdin) {
				if (errno == EINTR) {
					/*
					 * If we were interrupted by a
					 * signal (e.g. SIGTSTP), then
					 * we'll go ahead and restart.
					 */
					goto again;
				}
			} else {
				/* XXX report I/O error? */
			}
			clearerr(fp);
		}
	}
	return rv;
}

static void
jttb_putchar(void *vctx, void *vf, int ch)
{
	FILE *fp = (vf == TBVM_FILE_CONSOLE) ? stdout : vf;

	fputc(ch, fp);
}

static bool
jttb_check_break(void *vctx, void *vf)
{
	bool rv = false;

	if (vf == TBVM_FILE_CONSOLE) {
		/* XXX Not atomic. */
		if (received_sigint) {
			received_sigint = 0;
			rv = true;
		}
	}

	return rv;
}

static const struct tbvm_file_io jttb_file_io = {
	.io_openfile = jttb_openfile,
	.io_closefile = jttb_closefile,
	.io_getchar = jttb_getchar,
	.io_putchar = jttb_putchar,
	.io_check_break = jttb_check_break,
};

static bool
jttb_gettime(void *vctx, unsigned long *secsp)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		*secsp = ts.tv_sec;
		return true;
	}
	return false;
}

static const struct tbvm_time_io jttb_time_io = {
	.io_gettime = jttb_gettime,
};

static int
jttb_math_exc(void *vctx)
{
	int exc = (int)fp_exceptions;
	fp_exceptions = 0;

#ifndef __vax__
	int fpexc =
	    fetestexcept(FE_UNDERFLOW|FE_OVERFLOW|FE_DIVBYZERO|FE_INVALID);

	if (fpexc) {
		if (fpexc & FE_DIVBYZERO) {
			exc |= TBVM_EXC_DIV0;
		} else if (fpexc & (FE_UNDERFLOW|FE_OVERFLOW|FE_INVALID)) {
			exc |= TBVM_EXC_ARITH;
		}
		feclearexcept(FE_ALL_EXCEPT);
	}
#endif /* __vax__ */

	return exc;
}

static const struct tbvm_exc_io jttb_exc_io = {
	.io_math_exc = jttb_math_exc,
};

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	sigset_t nset;

	printf("%s, version %s\n", tbvm_name(), tbvm_version());

	/*
	 * We rely on SIGINT working properly in order to perform
	 * console BREAK processing.  Unfortunately, some shells
	 * seem to sometimes mask SIGINT?
	 */
	sigemptyset(&nset);
	sigaddset(&nset, SIGINT);
	if (sigprocmask(SIG_UNBLOCK, &nset, NULL) != 0) {
		abort();
	}
	signal(SIGINT, sigint_handler);

	sa.sa_sigaction = sigfpe_action;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGFPE, &sa, NULL);

	vm = tbvm_alloc(NULL);
	tbvm_set_file_io(vm, &jttb_file_io);
	tbvm_set_time_io(vm, &jttb_time_io);
	tbvm_set_exc_io(vm, &jttb_exc_io);
	tbvm_exec(vm);
	tbvm_free(vm);

	return 0;
}
