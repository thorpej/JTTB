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

#ifndef tbvm_h_included
#define	tbvm_h_included

/*
 * Interface defintions for the Tiny BASIC Virtual Machine.
 */

#include <stdbool.h>

struct tbvm;
typedef struct tbvm tbvm;

const char *tbvm_name(void);
const char *tbvm_version(void);

tbvm	*tbvm_alloc(void *);
void	tbvm_exec(tbvm *);
void	tbvm_free(tbvm *);

void	tbvm_set_prog(tbvm *, const char *, size_t);

#define	TBVM_EXC_DIV0		0x0001
#define	TBVM_EXC_ARITH		0x0002

#define	TBVM_FILE_CONSOLE	((void *)-1)

struct tbvm_file_io {
	void *	(*io_openfile)(void *, const char *, const char *);
	void	(*io_closefile)(void *, void *);
	int	(*io_getchar)(void *, void *);
	void	(*io_putchar)(void *, void *, int);
	bool	(*io_check_break)(void *, void *);
};

void	tbvm_set_file_io(tbvm *, const struct tbvm_file_io *);

struct tbvm_time_io {
	bool	(*io_gettime)(void *, unsigned long *);
};

void	tbvm_set_time_io(tbvm *, const struct tbvm_time_io *);

struct tbvm_exc_io {
	int	(*io_math_exc)(void *);
};

void	tbvm_set_exc_io(tbvm *, const struct tbvm_exc_io *);

#endif /* tbvm_h_included */
