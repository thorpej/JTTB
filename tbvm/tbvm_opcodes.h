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

/* JTTB additions */
#define	OPC_RUN		33
#define	OPC_EXIT	34
#define	OPC_CMPRX	35
#define	OPC_FOR		36
#define	OPC_STEP	37
#define	OPC_NXTFOR	38
#define	OPC_MOD		39
#define	OPC_POW		40
#define	OPC_RND		41
#define	OPC_ABS		42
#define	OPC_TSTEOL	43
#define	OPC_TSTS	44
#define	OPC_STR		45
#define	OPC_VAL		46
#define	OPC_HEX		47
#define	OPC_CPY		48
#define	OPC_LSTX	49
#define	OPC_STRLEN	50
#define	OPC_ASC		51
#define	OPC_CHR		52
#define	OPC_FIX		53
#define	OPC_SGN		54
#define	OPC_SCAN	55
#define	OPC_ONDONE	56
#define	OPC_ADVEOL	57
#define	OPC_INVAR	58
#define	OPC_POP		59
#define	OPC_LDPRG	60
#define	OPC_SVPRG	61
#define	OPC_DONEM	62
#define	OPC_SRND	63
#define	OPC_FLR		64
#define	OPC_CEIL	65
#define	OPC_ATN		66
#define	OPC_COS		67
#define	OPC_SIN		68
#define	OPC_TAN		69
#define	OPC_EXP		70
#define	OPC_LOG		71
#define	OPC_SQR		72
#define	OPC_MKS		73
#define	OPC_SBSTR	74

#define	OPC___LAST	OPC_SBSTR
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
