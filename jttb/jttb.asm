;
; Copyright (c) 2024 Jason R. Thorpe.
; All rights reserved.
;
; Redistribution and use in source and binary forms, with or without
; modification, are permitted provided that the following conditions
; are met:
; 1. Redistributions of source code must retain the above copyright
;    notice, this list of conditions and the following disclaimer.
; 2. Redistributions in binary form must reproduce the above copyright
;    notice, this list of conditions and the following disclaimer in the
;    documentation and/or other materials provided with the distribution.
;
; THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
; IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
; OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
; IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
; INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
; BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
; AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
; OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
; OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
; SUCH DAMAGE.
;

;
; Tiny BASIC virtual machine program
;
; This is based on the original Tiny BASIC VM program shown here:
;
; http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html
;
; It has been restructured slightly, for visual purposes and to make it
; easier to extend in the future.
;
; Changes / additions to the original Tiny BASIC functionality are
; listed here:
;
; ==> The RUN command uses a new RUN VM insn that explicitly changes mode.
;
; ==> Added an EXIT command that uses a new EXIT VM insn to cause the
;     VM to stop running bytecode and return to the driver.
;
; ==> Added a REM statement for comments.
;
; ==> Added NEW as an alias for CLEAR.
;
; ==> Added FOR / NEXT loops using new FOR, STEP, and NXTFOR VM insns.
;
; ==> Added exponentiation (^) and modulus (%) to expression evaluation
;     (TERM) using new EXP and MOD VM insns.
;
; ==> Added an RND() function using a new RND VM insn.
;
; ==> Added an ABS() function using a new ABS VM insn.
;
; ==> Make the PRINT statement more like MS BASIC's, using the new
;     TSTEOL VM insn.
;
; ==> Strings can now be part of expressions.  The VM does the requisite
;     type checking.
;
; ==> String variables are supported (e.g. A$ - Z$).  They occupy a separate
;     variable namespace than numeric variables.
;

;
; *** Main entry point
;
START:	INIT			; Initialize the VM.
	NLINE			; Print a blank line.

;
; *** Line collector
;
CO:	GETLINE			; Write prompt and get line.
	TSTL	XEC		; Test for line numer.
	INSRT			; Insert (or delete) line.
	JMP	CO		; Get another line.

;
; *** Statement executor
;
XEC:
STMT:	XINIT			; Initialize for a statement.

	;
	; LET var = expression
	;
	TST	notLET,'LET'	; LET statement?
	TSTV	Serr		; Yes, place VAR on AESTK
	TST	Serr,'='
	CALL	EXPR		; Place expression value on AESTK
	DONE			; End of statement.
	STORE			; Store result in VAR.
	NXT			; Next statement.
notLET:

	;
	; GOTO expression
	; GOSUB expression
	;
	TST	notGO,'GO'	; GOTO or GOSUB statement?
	TST	notGOTO,'TO'	; GOTO?
	CALL	EXPR		; Yes, get target.
	DONE			; End of statement.
	XFER			; Jump to target.
notGOTO:
	TST	Serr,'SUB'	; GOSUB?
	CALL	EXPR		; Yes, get target.
	DONE			; End of statement.
	SAV			; Save return location.
	XFER			; Jump to target.
notGO:

	;
	; PRINT printlist
	;
	; printlist ::=
	;               printitem
	;               printitem |
	;               printitem separator printlist
	;
	; printitem ::= "characterstring"
	;               expression
	;
	; separator ::= ,
	;               ;
	;
	TST	notPRINT,'PRINT'; PRINT statement?

	;
	; Check for bare PRINT statement, which just prints a newline.
	;
	TSTEOL	PR1
	NLINE			; Print newline.
	NXT			; Next statement.

	;
	; Handle EOL when there is a trailing separator, which does
	; not have a newline at the end.
	;
PREOLsep:
	TSTEOL	PR1		; Test for EOL.
	NXT			; Next statement.

PR1:	TST	PR2,'"'		; Test for quote.
	PRS			; Yes, print string.
	JMP	PRchecksep	; Go check for a separator.

PR2:	CALL	EXPR		; Get expression.
	PRN			; Print value.
	; FALLTHROUGH

PRchecksep:
	TST	PR3,','		; Test for comma separator.
	SPC			; Advance print head.
	JMP	PREOLsep	; Check for EOL and maybe process more.

PR3:	TST	PR4,';'		; Test for semicolon separator.
	JMP	PREOLsep	; Check for EOL and maybe process more.

	;
	; We've processed arguments and there is no separator; this means
	; we are done and that a newline should be printed.
	;
PR4:	DONE			; End of statement.
	NLINE			; Print newline.
	NXT			; Next statement.
notPRINT:

	;
	; IF expression relop expression THEN statement
	;
	TST	notIF,'IF'	; IF statement?
	CALL	EXPR		; Get first expression.
	CALL	RELOP		; Get relational operation.
	CALL	EXPR		; Get second expression.
	TST	Serr,'THEN'	; Check for THEN.
	CMPRX	IF1		; Perform comparsion
	JMP	STMT		; True, perform the statement.
IF1:	NXT			; Next statement.
notIF:

	;
	; FOR var = expression TO expression (STEP expression)
	;
	TST	notFOR,'FOR'	; FOR statement?
	TSTV	Serr		; Yes, get var address.
	TST	Serr,'='
	CALL	EXPR		; Get first expression.
	TST	Serr,'TO'
	CALL	EXPR		; Get second expression.
	FOR			; Push onto loop stack.
	TST	noSTEP,'STEP'	; Check for STEP.
	CALL	EXPR		; Get step expression.
	STEP			; Adjust STEP count from default.
noSTEP:
	DONE			; End of statement.
	NXT			; Next statement.
notFOR:

	;
	; NEXT var
	;
	TST	notNEXT,'NEXT'	; NEXT statement?
	TSTV	Serr		; Yes, get var address.
	DONE			; End of statement.
	NXTFOR			; Next statement according to loop cond.
notNEXT:

	;
	; INPUT var-list
	;
	; var-list::= var (, var)*
	;
	TST	notINPUT,'INPUT'; INPUT statement?
IN1:	TSTV	Serr		; Get var address.
	INNUM			; Get number from terminal.
	STORE			; Store it.
	TST	IN2,','		; More?
	JMP	IN1		; Yes, go get them.
IN2:	DONE			; End of statement.
	NXT			; Next statement.
notINPUT:

	;
	; RETURN
	;
	TST	notRTN,'RETURN'	; RETURN statement?
	DONE			; Yes, end of statement.
	RSTR			; Restore location.
	NXT			; Next statement.
notRTN:

	;
	; REM <rest of line ignored>
	;
	TST	notREM,'REM'	; REM statement?
	NXT			; Next statement.
notREM:

	;
	; END
	;
	TST	notEND,'END'	; END statement?
				; XXX missing DONE?
	FIN			; Yes, return to direct mode.
notEND:

	;
	; LIST
	;
	TST	notLIST,'LIST'	; LIST command?
	DONE			; Yes, end of statement.
	LST			; Go do it.
	NXT			; Next statement.
notLIST:

	;
	; RUN
	;
	TST	notRUN,'RUN'	; RUN command?
	DONE			; Yes, end of statement.
	RUN			; changes from direct mode
notRUN:

	;
	; CLEAR
	;
	TST	CLR1,'CLEAR'	; CLEAR command?
	JMP	CLR2
CLR1:	TST	notCLR,'NEW'	; NEW is a synonym.
CLR2:	DONE			; End of statement.
	JMP	START		; Re-initialize VM.
notCLR:

	;
	; EXIT
	;
	TST	notEXIT,'EXIT'	; EXIT command?
	DONE			; End of statement.
	EXIT
notEXIT:

Serr:	ERR			; Syntax error.

;
; *** Expression evaluation
;
; expression ::= unsignedexpr
;                + unsignedexpr
;                - unsignedexpr
;
; unsignedexpr ::= term
;                  term + unsignedexpr
;                  term - unsignedexpr
;
; term ::= factor
;          factor * term
;          factor / term
;          factor ^ term
;          factor % term
;
; factor ::= function
;            var
;            "characterstring"
;            number
;            ( expression )
;
; function ::= RND ( expression )
;              ABS ( expression )
;
; var ::= A | B | ... | Y | Z
;
; number ::= digit
;            digit number
;
; digit ::= 0 | 1 | 2  | ...  | 8 | 9
;
EXPR:	TST	E0,'-'		; Unary -?
	CALL	TERM		; Yes, get first term.
	NEG			; Negate it.
	JMP	E1

	;
	; This test for unary + lands at the same VM program
	; address regardless if the + exists or not.  Basically,
	; the TST insn here just serves to notice and skip over
	; it.
	;
E0:	TST	E0A,'+'		; Unary +?
E0A:	CALL	TERM		; Get first term.

E1:	TST	E2,'+'		; Sum?
	CALL	TERM		; Yes, get second term.
	ADD
	JMP	E1		; Check for more.

E2:	TST	E3,'-'		; Difference?
	CALL	TERM		; Yes, get second term.
	SUB
	JMP	E1		; Check for more.

E3: T4:	RTN			; All done.

TERM:	CALL	FACT		; Get first factor.

T0:	TST	T1,'*'		; Product?
	CALL	FACT		; Get second factor.
	MUL
	JMP	T0		; Check for more.

T1:	TST	T2,'/'		; Quotient?
	CALL	FACT		; Get second factor.
	DIV
	JMP	T0		; Check for more.

T2:	TST	T3,'^'		; Exponentiation?
	CALL	FACT		; Get second factor.
	EXP
	JMP	T0		; Check for more.

T3:	TST	T4,'%'		; Modulus?
	CALL	FACT		; Get second factor.
	MOD
	JMP	T0		; Check for more.

FACT:
	;
	; We have to check for functions first, because the first
	; letter of a function name would match a variable.
	;
	TST	notRND,'RND'	; RND() function?
	TST	Serr,'('
	CALL	EXPR		; Get range expression.
	TST	Serr,')'
	RND
	RTN
notRND:

	TST	notABS,'ABS'	; ABS() function?
	TST	Serr,'('
	CALL	EXPR		; Get expression.
	TST	Serr,')'
	ABS
	RTN
notABS:

	TST	notVAL,'VAL'	; VAL() function?
	TST	Serr,'('
	CALL	EXPR		; Get expression.
	TST	Serr,')'
	VAL
	RTN
notVAL:

	TST	notSTR,'STR$'	; STR$() function?
	TST	Serr,'('
	CALL	EXPR		; Get expression.
	TST	Serr,')'
	STR
	RTN
notSTR:

	TSTV	F0		; Variable?
	IND			; Yes, get the value.
	RTN

F0:	TSTS	F1		; String?  Push it onto the stack.
	RTN

F1:	TSTN	F2		; Number?  Push it onto the stack.
	RTN

F2:	TST	F3,'('		; Parenthesized expression?
	CALL	EXPR		; Go evaluate it.
	TST	F2,')'
	RTN

F3:	ERR			; Syntax error.

;
; *** Relational operations
;
; relop::= < (> | = | e) | > (< | = | e) | =
;
RELOP:	TST	R0,'='
	LIT	0		; = EQUAL
	RTN

R0:	TST	R4,'<'
	TST	R1,'='
	LIT	2		; <= LESS-THAN-OR-EQUAL
	RTN

R1:	TST	R3,'>'
	LIT	3		; <> NOT-EQUAL
	RTN

R3:	LIT	1		; < LESS-THAN
	RTN

R4:	TST	Serr,'>'
	TST	R5,'='
	LIT	5		; >= GREATER-THAN-OR-EQUAL
	RTN

R5:	TST	R6,'<'
	LIT	3		; >< NOT-EQUAL (synonym)
	RTN

R6:	LIT	4		; > GREATER-THAN
	RTN
