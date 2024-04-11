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
XEC:	XINIT			; Initialize for a statement.
STMT:				; XXX Should this == XEC?

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
	; PRINT expr-list
	;
	; expr-list::= (string | expression) (, (string | expression) )*
	;
	TST	notPRINT,'PRINT'; PRINT statement?
PR1:	TST	PR4,'"'		; Test for quote.
	PRS			; Yes, print string.
PR2:	TST	PR3,','		; Is there more?
	SPC			; Yes, space to next zone.
	JMP	PR1		; Get more stuff to print.
PR3:	DONE			; End of statement.
	NLINE
	NXT			; Next statement.

PR4:	CALL	EXPR		; Get expression
	PRN			; Print value.
	JMP	PR2		; Check for more.
notPRINT:

	;
	; IF expression relop expression THEN statement
	;
	TST	notIF,'IF'	; IF statement?
	CALL	EXPR		; Get first expression.
	CALL	RELOP		; Get relational operation.
	CALL	EXPR		; Get second expression.
	TST	Serr,'THEN'	; Check for THEN.
	CMPR			; Perform comparsion - performs NXT if false.
	JMP	STMT		; True, perform the statement.
notIF:

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
	TST	notCLR,'CLEAR'	; CLEAR command?
	DONE			; End of statement.
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
; expression::= (+ | - | e) term ((+ | -) term)*
; term::= factor ((* | /) factor)*
; factor::= var | number | (expression)
; var::= A | B | C ..., | Y | Z
; number::= digit digit*
; digit::= 0 | 1 | 2  | ...  | 8 | 9
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

E3: T2:	RTN			; All done.

TERM:	CALL	FACT		; Get first factor.

T0:	TST	T1,'*'		; Product?
	CALL	FACT		; Get second factor.
	MUL
	JMP	T0		; Check for more.

T1:	TST	T2,'/'		; Quotient?
	CALL	FACT		; Get second factor.
	DIV
	JMP	T0		; Check for more.

FACT:	TSTV	F0		; Variable?
	IND			; Yes, get the value.
	RTN

F0:	TSTN	F1		; Number?  Get it's value.
	RTN

F1:	TST	F2,'('		; Parenthesized expression?
	CALL	EXPR		; Go evaluate it.
	TST	F2,')'
	RTN

F2:	ERR			; Syntax error.


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
