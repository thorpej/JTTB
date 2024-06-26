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
; ==> Renamed the original CLEAR as NEW.  Changed CLEAR to match the
;     MS BASIC definition of allocating string and program space.
;
; ==> Added FOR / NEXT loops using new FOR, STEP, and NXTFOR VM insns.
;
; ==> Added exponentiation (^) and modulus (%) to expression evaluation
;     (TERM) using new POW and MOD VM insns.
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
; ==> Added a STR$() function using a new STR VM insn.
;
; ==> Added a VAL() function using a new VAL VM insn.
;
; ==> Added a HEX$() function using a new HEX VM insn.
;
; ==> PRINT no longer directly processes immediate strings; all PRINTs
;     now evaluate expressions (including expressions with strings).
;
; ==> The LIST statement now optionally takes line number ranges.  This
;     is implemented using the new CPY and LSTX VM insns.
;
; ==> Added a LEN() function using a new STRLEN VM insn.
;
; ==> Added an ASC() fuction using a new ASC VM insn.
;
; ==> Added a CHR$() function using a new CHR VM insn.
;
; ==> Added an INT() function using a new FIX VM insn.
;
; ==> Added a SGN() function using a new SGN VM insn.
;
; ==> Added support for ELSE branches if IF statements.  The SCAN,
;     ONDONE, and ADVEOL VM insns are used for this.
;
; ==> Added INVAR and POP VM insns, used to support both string and
;     integer variables in the INPUT statement.
;
; ==> Added optional prompt support to INPUT, using PRS VM insn.
;
; ==> Adjusted the line collector routine so that blank lines do not
;     result in a syntax error (they are merely ignored).
;
; ==> Implemented the LOAD command to load a progam into the program
;     store using the new LDPRG VM insn.
;
; ==> Implemented the SAVE command to save the program in the program
;     store using the new SVPRG VM insn.
;
; ==> Added a new DONEM VM insn that, when used, limits commands to
;     RUN-mode or DIRECT-mode.
;
; ==> Adjusted the parser to allow bare variable assignments without
;     the LET keyword.
;
; ==> Adjusted the parser to allow bare numbers after THEN and ELSE,
;     which are treated as an implied GOTO.
;
; ==> Added FIX() as another name for INT().
;
; ==> Added FLOOR() and CEIL() functions using new FLR and CEIL VM insns.
;
; ==> Added ATN(), COS(), SIN(), and TAN() functions using new ATN,
;     COS, SIN, and TAN VM insns.
;
; ==> Added EXP(), LOG(), and SQR() functions using new EXP, LOG, and
;     SQR VM insns.
;
; ==> Added the STRING$() function using the new MKS VM insn.
;
; ==> Added LEFT$(), MID$(), and RIGHT$() functions using the new
;     SBSTR VM insn.
;
; ==> Added DATA, READ, and RESTORE statements using new TSTSOL, NXTLN,
;     DMODE, and DSTORE VM insns.
;
; ==> Added support for dimensioned arrays using new DIM and ARRY VM
;     insns.
;
; ==> Added support for the TAB() and SPC() functions using the new
;     ADVCRS VM insn.
;
; ==> Added support for the DEG() and RAD() functions using the new
;     DEGRAD VM insn.
;
; ==> Added support for the Standard BASIC keyword PI, which is
;     implemented by substituting RAD(180).
;
; ==> Added Standard BASIC UCASE$() and LCASE$() functions that convert
;     a string to all-upper-case or all-lower-case, respectively, using
;     the new UPRLWR VM insn.
;
; Original Tiny BASIC VM opcodes that are no longer used:
; ==> CMPR (replaced by CMPRX)
; ==> LST (replaced by LSTX)
; ==> INNUM (replaced by INVAR)
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
	TSTL	CO1		; Test for line numer.
	INSRT			; Insert (or delete) line.
	JMP	CO		; Get another line.
CO1:
	TSTEOL	XEC		; Check for EOL (blank line).
	JMP	CO		; Yup, just get another line.

;
; *** Statement executor
;
XEC:
STMT:	XINIT			; Initialize for a statement.

	;
	; LET var = expression
	;
	TST	notLET,'LET'	; LET statement?
	CALL	ReqVarOrArray
isLET:	TST	Serr,'='
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
isGOTO:	DONEM	0		; End of statement (RUN-mode).
	XFER			; Jump to target.
notGOTO:
	TST	Serr,'SUB'	; GOSUB?
	CALL	EXPR		; Yes, get target.
	DONEM	0		; End of statement (RUN-mode).
	SAV			; Save return location.
	XFER			; Jump to target.
notGO:

	;
	; PRINT printlist
	;
	; printlist ::=
	;               expression
	;               expression separator printlist
	;
	; separator ::= ,
	;               ;
	;
	TST	notPRINT,'PRINT'; PRINT statement?

	;
	; Check for bare PRINT statement, which just prints a newline.
	;
	TSTEOL	PR1
	DONE			; End of statement.
	NLINE			; Print newline.
	NXT			; Next statement.

	;
	; Handle EOL when there is a trailing separator, which does
	; not have a newline at the end.
	;
PREOLsep:
	TSTEOL	PR1		; Test for EOL.
	DONE			; End of statement.
	NXT			; Next statement.

PR1:	CALL	EXPR		; Get expression.
	PRN			; Print value.
	; FALLTHROUGH

PRchecksep:
	TST	PR2,','		; Test for comma separator.
	SPC			; Advance print head.
	JMP	PREOLsep	; Check for EOL and maybe process more.

PR2:	TST	PR3,';'		; Test for semicolon separator.
	JMP	PREOLsep	; Check for EOL and maybe process more.

	;
	; We've processed arguments and there is no separator; this means
	; we are done and that a newline should be printed.
	;
PR3:	DONE			; End of statement.
	NLINE			; Print newline.
	NXT			; Next statement.
notPRINT:

	;
	; IF expression relop expression THEN statement elsebranch
	;
	; elsebranch ::=
	;                ELSE statement
	;
	; N.B. the parser is not very sophisticated here, and not
	; particularly tolerant of ambiguous scoping.  For example:
	;
	;	IF F = 12 THEN IF G = 13 GOTO 100 ELSE GOTO 200
	;
	; In this case, the "GOTO 200" is going to be executed if "F = 12"
	; is false, which may not be what is intended.  Furthermore, the
	; parser will flag a syntax error if another ELSE branch is
	; placed after "GOTO 200".
	;
	; On the other hand:
	;
	;	IF F = 12 then GOTO 100 ELSE IF G = 13 GOTO 200 ELSE GOTO 300
	;
	; ...this will work in the expected manner, which is if "F = 12"
	; is false, then "IF G = 13 GOTO 200 ELSE GOTO 300" will be the
	; next statement executed, which does not have any ambiguity in
	; the scoping.
	;
	TST	notIF,'IF'	; IF statement?
	CALL	EXPR		; Get first expression.
	CALL	RELOP		; Get relational operation.
	CALL	EXPR		; Get second expression.
	TST	Serr,'THEN'	; Check for THEN.
	CMPRX	IF1		; Perform comparsion
	ONDONE	IF3		; True, hook into DONE to skip possible ELSE.
	;
	; ***** Special case *****
	; If we find a bare number after THEN, it's an implied GOTO.
	;
	TSTN	IFT1		; Check for number.
	JMP	isGOTO		; Go process implied GOTO.
IFT1:	JMP	STMT		; Perform the statement.
IF1:	SCAN	IF2,'ELSE'	; False, scan for an ELSE...
	;
	; ***** Special case *****
	; If we find a bare number after ELSE, it's an implied GOTO.
	;
	TSTN	IFE1		; Check for number.
	JMP	isGOTO		; Go process implied GOTO.
IFE1:	JMP	STMT		; ...and perform that statement.

	;
	; We can't really perform a DONE here.  If we do, we're just
	; going to bump into the statement in the THEN branch that
	; wasn't taken.
	;
IF2:	NXT			; Next statement.

	;
	; This is the ONDONE hook that takes care of skipping an
	; ELSE branch, if present.
	;
IF3:	TST	IF4,'ELSE'	; Is there an ELSE branch to skip?
	ADVEOL			; Yes, advance past it.
IF4:	RTN			; Return so the DONE can proceed.
notIF:

	;
	; FOR var = expression TO expression forstep
	;
	; forstep ::=
	;             STEP expression
	;
	TST	notFOR,'FOR'	; FOR statement?
	CALL	ReqVarOrArray
	TST	Serr,'='
	CALL	EXPR		; Get first expression.
	TST	Serr,'TO'
	CALL	EXPR		; Get second expression.
	FOR			; Push onto loop stack.
	TST	noSTEP,'STEP'	; Check for STEP.
	CALL	EXPR		; Get step expression.
	STEP			; Adjust STEP count from default.
noSTEP:
	DONEM	0		; End of statement (RUN-mode).
	NXT			; Next statement.
notFOR:

	;
	; NEXT opt-var
	;
	; opt-var ::=
	;             var
	;
	TST	notNEXT,'NEXT'	; NEXT statement?
	TSTEOL	NXT1		; Check for bare NEXT statement.
	LIT	0		; Yes, make a note for NXTFOR.
	JMP	NXT2
NXT1:	CALL	ReqVarOrArray	; Otherwise, a var is required.
NXT2:	DONEM	0		; End of statement (RUN-mode).
	NXTFOR			; Next statement according to loop cond.
notNEXT:

	;
	; INPUT opt-prompt var-list
	;
	; opt-prompt ::=
	;                "characterstring" ;
	;
	; var-list ::=
	;              var , var-list
	;
	TST	notINPUT,'INPUT'; INPUT statement?
	TST	IN1,'"'		; Prompt string?
	PRS			; Yes, print it.
	TST	Serr,';'	; Require ; separator between string and var.
IN1:	LIT	1		; Start with 1 input prompt char.
IN2:	CALL	ReqVarOrArray
	INVAR			; Get value from terminal and store it.
	TST	IN3,','		; More?
	LIT	1		; Yes, add 1 to the prompt count.
	ADD
	JMP	IN2		; Yes, go get them.
IN3:	DONE			; End of statement.
	POP			; Pop prompt char count.
	NXT			; Next statement.
notINPUT:

	;
	; RETURN
	;
	TST	notRTN,'RETURN'	; RETURN statement?
	DONEM	0		; Yes, end of statement (RUN-mode).
	RSTR			; Restore location.
	NXT			; Next statement.
notRTN:

	;
	; REM <rest of line ignored>
	;
	; DATA statements are treated the same way in the statement
	; executor.
	;
	TST	REM1,'REM'	; REM statement?
	JMP	REM2
REM1:	TST	notREM,'DATA'	; DATA statements ignored in executor.
REM2:	ADVEOL			; Skip to the end of line.
	DONE			; End of statement.
	NXT			; Next statement.
notREM:

	;
	; READ var-list
	;
	; var-list ::=
	;              var , var-list
	;
	TST	notREAD,'READ'	; READ statement?
RD1:	CALL	ReqVarOrArray
	DMODE	1		; Goto into DATA mode.
	TSTSOL	RD3		; At start-of-line?
RD2:	TST	RDnxt,'DATA'	; Yes, DATA statement?
RDstor:	DSTORE			; Yes, store data in var.
	DMODE	0		; Exit data mode.
	TST	RDDone,','	; More vars?
	JMP	RD1		; Yes, to get them.
RD3:	TST	RD4,','		; Separator?
	JMP	RDstor		; Yes, go store data.
RD4:	TSTEOL	Serr		; If not end-of-line, syntax error.
RDnxt:	NXTLN	RDDerr		; Advance to next line, if available.
	JMP	RD2		; Try again.
RDDerr:	DMODE	2		; Out of data error.
RDDone:	DONEM	0		; End of statement (RUN-mode).
	NXT			; Next statement.
notREAD:

	;
	; RESTORE
	;
	TST	notRSTR,'RESTORE' ; RESTORE statement?
	DONEM	0		; End of statement (RUN-mode).
	DMODE	3		; Restore data pointer.
	NXT			; Next statement.
notRSTR:

	;
	; END
	;
	TST	notEND,'END'	; END statement?
	; The original Tiny BASIC VM program did not have a DONE
	; here, because there's no point; the program is ending.
	; XXX Maybe put one here to be pedantic about syntax errors?
	FIN			; Yes, return to direct mode.
notEND:

	;
	; LIST
	; LIST line
	; LIST firstline -
	; LIST - lastline
	; LIST firstline - lastline
	;
	; N.B. These are NOT expressions!  Only numbers are allowed.
	;
	TST	notLIST,'LIST'	; LIST command?
	TSTEOL	LST1		; Check for arguments.
	LIT	0		; No arguments, pass 0,0 to indicate whole
	LIT	0		; program.
LST99:	DONEM	1		; End of statement (DIRECT-mode).
	LSTX			; Go do it.
	NXT			; Next statement.

LST1:	TSTN	LST4		; Check for first line number.
LST5:	TST	LST3,'-'	; Check for range separator.
	TSTN	LST2		; Check for last line number.
	JMP	LST99		; Go do it.

LST2:				; No last line number
	LIT	0		; Pass 0 to indicate end-of-program.
	JMP	LST99		; Go do it.

LST3:				; No range separator
	CPY			; Copy first line to last line.
	JMP	LST99		; Go do it.

LST4:	LIT	0		; No first line, push 0.
	JMP	LST5		; Go check for additional arguments.
notLIST:

	;
	; RUN
	;
	TST	notRUN,'RUN'	; RUN command?
	DONEM	1		; Yes, end of statement (DIRECT-mode).
	RUN			; changes from direct mode
notRUN:

	;
	; NEW
	;
	TST	notNEW,'NEW'	; NEW command?
	DONEM	1		; End of statement (DIRECT-mode).
	JMP	START		; Re-initialize VM.
notNEW:

	;
	; CLEAR number opt-highmem
	;
	; opt-highmem ::=
	;                 , number
	;
	; We don't need to do anything about this in this interpreter
	; for the moment.  We do enforce the MS BASIC syntax, though.
	;
	TST	notCLR,'CLEAR'	; CLEAR statement?
	TSTN	Serr		; Number required.
	POP			; Pop the number we're not going to use.
	TST	CLR99,','	; Separator for optional highmem arg?
	TSTN	Serr		; Yes, number required.
	POP			; Pop the number we're not going to use.
CLR99:	DONEM	0		; End of statement (RUN-mode).
	NXT			; Next statement.
notCLR:

	;
	; DIM dim-var-list
	;
	; dim-var-list ::=
	;                  dim-var , dim-var-list
	;
	; dim-var ::= var ( expression opt-dim )
	;
	; opt-dim ::=
	;             , expression opt-dim
	;
	TST	notDIM,'DIM'	; DIM statement?
DIM0:	TSTV	Serr		; Get naming var.
	TST	Serr,'('
DIM1:	CALL	EXPR		; Get expression argument.
	TST	DIM2,','	; Separator?
	JMP	DIM1		; Get, get next expression argument.
DIM2:	TST	Serr,')'
	DIM			; Dimension array associated with var.
	TST	DIM3,','	; Another array to dimension?
	JMP	DIM0		; Yes, go do it.
DIM3:	DONE			; End of statement.
	NXT			; Next statement.
notDIM:

	;
	; LOAD "characterstring"
	;
	TST	notLOAD,'LOAD'	; LOAD command?
	TSTS	LD1		; Push file name onto AESTK.
	JMP	LD2
LD1:	LIT	0		; 0 -> no program name
LD2:	DONEM	1		; End of statement (DIRECT-mode).
	LDPRG			; Go load the program. Returns to direct mode.
notLOAD:

	;
	; SAVE "characterstring"
	;
	TST	notSAVE,'SAVE'	; SAVE command?
	TSTS	SV1		; Push file name onto AESTK.
	JMP	SV2
SV1:	LIT	0		; 0 -> no program name
SV2:	DONEM	1		; End of statement (DIRECT-mode).
	SVPRG			; Go save the program. Returns to direct mode.
notSAVE:

	;
	; EXIT
	;
	TST	notEXIT,'EXIT'	; EXIT command?
	DONE			; End of statement.
	EXIT
notEXIT:

	;
	; RANDOMIZE opt-expr
	;
	; opt-expr ::=
	;              expr
	;
	TST	notSRND,'RANDOMIZE' ; RANDOMIZE command?
	TSTEOL	SRND1		; Check for EOL.
	LIT	0		; Yes, push 0.
	JMP	SRND2		; Go finish.
SRND1:	CALL	EXPR		; Get expression.
SRND2:	DONE			; End of statement.
	SRND			; Seed random number generator.
	NXT			; Next statement.
notSRND:

	;
	; ***** LAST CASE BEFORE FALLING THROUGH TO Serr *****
	; Look for a variable name and, if it looks like we have
	; one, jump into the middle of LET.  This will almost
	; always match, but a real bogus statement will lack the
	; correct syntax to match LET.
	;
	; This is to match the extremely-common-among-BASICs behavior
	; of allowing variable assignments without LET.
	;
	CALL	ReqVarOrArray	; Check for a var or array.
	JMP	isLET		; ...and try to match an assignment.

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
;            reserved-const
;            var
;            "characterstring"
;            number
;            ( expression )
;
; function ::= RND ( expression )
;              ABS ( expression )
;              ASC ( expression )
;              ATN ( expression )
;              COS ( expression )
;              VAL ( expression )
;              LEN ( expression )
;              FIX ( expression )
;              INT ( expression )
;              FLOOR ( expression )
;              CEIL ( expression )
;              SGN ( expression )
;              SIN ( expression )
;              TAN ( expression )
;              EXP ( expression )
;              LOG ( expression )
;              SQR ( expression )
;              SPC ( expression )
;              TAB ( expression )
;              CHR$ ( expression )
;              STR$ ( expression )
;              HEX$ ( expression )
;              STRING$ ( expression , expression )
;              MID$ ( expression , expression mid-opt-len )
;              RIGHT$ ( expression , expression )
;              LEFT$ ( expression , expression )
;              UCASE$ ( expression )
;              LCASE$ ( expression )
;
; mid-opt-len ::=
;                 , expression
;
; reserved-const ::= PI
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
	POW
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
	CALL	FUNC1ARG
	RND
	RTN
notRND:

	TST	notABS,'ABS'	; ABS() function?
	CALL	FUNC1ARG
	ABS
	RTN
notABS:

	TST	notASC,'ASC'	; ASC() function?
	CALL	FUNC1ARG
	ASC
	RTN
notASC:

	TST	notATN,'ATN'	; ATN() function?
	CALL	FUNC1ARG
	ATN
	RTN
notATN:

	TST	notCOS,'COS'	; COS() function?
	CALL	FUNC1ARG
	COS
	RTN
notCOS:

	TST	notVAL,'VAL'	; VAL() function?
	CALL	FUNC1ARG
	VAL
	RTN
notVAL:

	TST	notLEN,'LEN'	; LEN() function?
	CALL	FUNC1ARG
	STRLEN
	RTN
notLEN:

	TST	FIX1,'FIX'	; FIX() function?
	JMP	FIX2
FIX1:	TST	notFIX,'INT'	; INT() is an alias
FIX2:	CALL	FUNC1ARG
	FIX
	RTN
notFIX:

	TST	notFLOOR,'FLOOR' ; FLOOR() function?
	CALL	FUNC1ARG
	FLR
	RTN
notFLOOR:

	TST	notCEIL,'CEIL'	; CEIL() function
	CALL	FUNC1ARG
	CEIL
	RTN
notCEIL:

	TST	notSGN,'SGN'	; SGN() function?
	CALL	FUNC1ARG
	SGN
	RTN
notSGN:

	TST	notSIN,'SIN'	; SIN() function?
	CALL	FUNC1ARG
	SIN
	RTN
notSIN:

	TST	notTAN,'TAN'	; TAN() function?
	CALL	FUNC1ARG
	TAN
	RTN
notTAN:

	TST	notEXP,'EXP'	; EXP() function?
	CALL	FUNC1ARG
	EXP
	RTN
notEXP:

	TST	notLOG,'LOG'	; LOG() function?
	CALL	FUNC1ARG
	LOG
	RTN
notLOG:

	TST	notSQR,'SQR'	; SQR() function?
	CALL	FUNC1ARG
	SQR
	RTN
notSQR:

	TST	notDEG,'DEG'	; DEG() function?
	CALL	FUNC1ARG
	DEGRAD	0		; mode 0 -> radians to degrees
	RTN
notDEG:

	TST	notRAD,'RAD'	; RAD() function?
	CALL	FUNC1ARG
	DEGRAD	1		; mode 1 -> degrees to radians
	RTN
notRAD:

	;
	; Classical MS BASIC does not process SPC() and TAB() like
	; normal functions, but rather looks for them inline with
	; PRINT processing.  We treat them like normal functions,
	; and they simply return an empty string after doing their
	; work.
	;

	TST	notSPC,'SPC'	; SPC() function?
	CALL	FUNC1ARG
	ADVCRS	0
	RTN
notSPC:

	TST	notTAB,'TAB'	; TAB() function?
	CALL	FUNC1ARG
	ADVCRS	1
	RTN
notTAB:

	TST	notCHR,'CHR$'	; CHR$() function?
	CALL	FUNC1ARG
	CHR
	RTN
notCHR:

	TST	notSTR,'STR$'	; STR$() function?
	CALL	FUNC1ARG
	STR
	RTN
notSTR:

	TST	notSTNG,'STRING$' ; STRING$() function?
	TST	Serr,'('
	CALL	EXPR		; First argument is numeric expression.
	TST	Serr,','
	CALL	EXPR		; Second argument is string or numeric expr.
	TST	Serr,')'
	MKS			; Make the string.
	RTN
notSTNG:

	TST	notHEX,'HEX$'	; HEX$() function?
	CALL	FUNC1ARG
	HEX
	RTN
notHEX:

	TST	notMID,'MID$'	; MID$() function?
	TST	Serr,'('
	CALL	EXPR		; First argument is string expression.
	TST	Serr,','
	CALL	EXPR		; Second argument is numeric expression.
	TST	MID1,','	; Third argument?
	CALL	EXPR		; Yes, and it's a numeric expression.
	LIT	0		; 3 args == SBSTR mode 0.
	JMP	MID2		; Go do it.
MID1:	LIT	1		; 2 args == SBSTR mode 1.
MID2:	TST	Serr,')'
	SBSTR
	RTN
notMID:

	TST	notRIGHT,'RIGHT$' ; RIGHT$() function?
	TST	Serr,'('
	CALL	EXPR		; First argument is string expression.
	TST	Serr,','
	CALL	EXPR		; Second argument is numeric expression.
	LIT	2		; SBSTR mode 2.
	JMP	MID2
notRIGHT:

	TST	notLEFT,'LEFT$'	; LEFT$() function?
	TST	Serr,'('
	CALL	EXPR		; First argument is string expression.
	LIT	1		; Position argument is 1 for SUBSTR 0.
	TST	Serr,','
	CALL	EXPR		; Second argument is numeric expression.
	LIT	0		; SUBSTR mode 0.
	JMP	MID2
notLEFT:

	TST	notUCASE,'UCASE$' ; UCASE$() function?
	CALL	FUNC1ARG
	UPRLWR	1		; mode 1 -> up-case
	RTN
notUCASE:

	TST	notLCASE,'LCASE$' ; LCASE$() function?
	CALL	FUNC1ARG
	UPRLWR	0		; mode 0 -> down-case
	RTN
notLCASE:

	;
	; Check for reserved constants before variables, because
	; these reserved names may otherwise collide with var
	; names.
	;
	TST	notPi,'PI'	; Is it Pi?
	LIT	180		; Yes, push 180 onto the expression stack.
	DEGRAD	1		; mode 1 -> degrees to radians
	RTN
notPi:

	TSTV	F0		; Variable?
	CALL	ARRAY		; Maybe index array.
	IND			; Yes, get the value.
	RTN

F0:	TSTS	F1		; String?  Push it onto the stack.
	RTN

F1:	TSTN	F2		; Number?  Push it onto the stack.
	RTN

FUNC1ARG:
	;
	; The the argument for a 1-argument function.  This is exactly
	; the same as processing a parenthesized expression.
	;
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

ReqVarOrArray:
	TSTV	Serr		; Var required, error if not present.
	;
	; Fall-through to check for an array.
	;
;
; *** Check for array index
;
ARRAY:	TST	AR99,'('
AR1:	CALL	EXPR		; Get expression.
	TST	AR98,','	; Separator?
	JMP	AR1		; Yes, get next expression.
AR98:	TST	Serr,')'
	ARRY			; Index the array.
AR99:	RTN
