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

#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

#include "../tbvm/tbvm.h"
#include "jttb_vmprog.h"

const char version[] = "0.1";

static tbvm	*vm;

static void
sigint_handler(int sig)
{
	tbvm_break(vm);
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

	return fgetc(fp);
}

static void
jttb_putchar(void *vctx, void *vf, int ch)
{
	FILE *fp = (vf == TBVM_FILE_CONSOLE) ? stdout : vf;

	fputc(ch, fp);
}

static const struct tbvm_file_io jttb_file_io = {
	.io_openfile = jttb_openfile,
	.io_closefile = jttb_closefile,
	.io_getchar = jttb_getchar,
	.io_putchar = jttb_putchar,
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

int
main(int argc, char *argv[])
{
	printf("Jason's Tiny BASIC, version %s\n", version);

	signal(SIGINT, sigint_handler);

	vm = tbvm_alloc(NULL);
	tbvm_set_prog(vm, tbvm_program, sizeof(tbvm_program));
	tbvm_set_file_io(vm, &jttb_file_io);
	tbvm_set_time_io(vm, &jttb_time_io);
	tbvm_exec(vm);
	tbvm_free(vm);

	return 0;
}
