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

#ifndef tbvm_opcodes_h_included
#define	tbvm_opcodes_h_included

/*
 * Opcode definitions for the Tiny BASIC virtual machine.
 *
 * http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html
 *
 * N.B. we are *not* using the "One Possible IL Encoding" described in the
 * article.
 */

#define	OPC_TST		0
#define	OPC_CALL	1
#define	OPC_RTN		2
#define	OPC_DONE	3
#define	OPC_JMP		4
#define	OPC_PRS		5
#define	OPC_PRN		6
#define	OPC_SPC		7
#define	OPC_NLINE	8
#define	OPC_NXT		9
#define	OPC_XFER	10
#define	OPC_SAV		11
#define	OPC_RSTR	12
#define	OPC_CMPR	13
#define	OPC_LIT		14
#define	OPC_INNUM	15
#define	OPC_FIN		16
#define	OPC_ERR		17
#define	OPC_ADD		18
#define	OPC_SUB		19
#define	OPC_NEG		20
#define	OPC_MUL		21
#define	OPC_DIV		22
#define	OPC_STORE	23
#define	OPC_TSTV	24
#define	OPC_TSTN	25
#define	OPC_IND		26
#define	OPC_LST		27
#define	OPC_INIT	28
#define	OPC_GETLINE	29
#define	OPC_TSTL	30
#define	OPC_INSRT	31
#define	OPC_XINIT	32
#define	OPC_RUN		33	/* JTTB addition */
#define	OPC_EXIT	34	/* JTTB addition */

#define	OPC___LAST	OPC_EXIT
#define	OPC___COUNT	(OPC___LAST + 1)

#define	OPC_F_LABEL	0x01
#define	OPC_F_STRING	0x02
#define	OPC_F_NUMBER	0x04

#define	OPC_NUM_SIZE	1
#define	OPC_NUM_MIN	0
#define	OPC_NUM_MAX	255

#define	OPC_LBL_SIZE	2

#define	OPC_LBL_ABS	1
#define	OPC_LBL_REL	0

#endif /* tbvm_opcodes_h_included */
