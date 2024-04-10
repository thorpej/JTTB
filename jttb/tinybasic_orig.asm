    ;THE IL CONTROL SECTION

    START:  INIT                  ;INITIALIZE
            NLINE                 ;WRITE CRLF
    CO:     GETLINE               ;WRITE PROMPT AND GET LINE
            TSTL    XEC           ;TEST FOR LINE NUMBER
            INSRT                 ;INSRT IT (MAY BE DELETE)
            JMP     CO
    XEC:    XINIT                 ;INITIALIZE

    ;STATEMENT EXECUTOR

    STMT:   TST     S1,'LET'      ;IS STATEMENT A LET
            TSTV    S17           ;YES, PLACE VAR ADDRESS ON AESTK
            TST     S17,'='       ;(This line originally omitted)
            CALL    EXPR          ;PLACE EXPR VALUE ON AESTK
            DONE                  ;REPORT ERROR IF NOT NEXT
            STORE                 ;STORE RESULT
            NXT                   ;AND SEQUENCE TO NEXT
    S1:     TST     S3,'GO'       ;GOTO OT GOSUB?
            TST     S2,'TO'       ;YES...TO, OR...SUB
            CALL    EXPR          ;GET LABEL
            DONE                  ;ERROR IF CR NOT NEXT
            XFER                  ;SET UP AND JUMP
    S2:     TST     S17,'SUB'     ;ERROR IF NO MATCH
            CALL    EXPR          ;GET DESTINATION
            DONE                  ;ERROR IF CR NOT NEXT
            SAV                   ;SAVE RETURN LINE
            XFER                  ;AND JUMP
    S3:     TST     S8,'PRINT'    ;PRINT
    S4:     TST     S7,'"'        ;TEST FOR QUOTE
            PRS                   ;PRINT STRING
    S5:     TST     S6,','        ;IS THERE MORE?
            SPC                   ;SPACE TO NEXT ZONE
            JMP     S4            ;YES JUMP BACK
    S6:     DONE                  ;ERROR IF CR NOT NEXT
            NLINE
            NXT
    S7:     CALL    EXPR
            PRN                   ;PRINT IT
            JMP     S5            ;IS THERE MORE?
    S8:     TST     S9,'IF'       ;IF STATEMENT
            CALL    EXPR          ;GET EXPRESSION
            CALL    RELOP         ;DETERMINE OPR AND PUT ON STK
            CALL    EXPR          ;GET EXPRESSION
            TST     S17,'THEN'    ;(This line originally omitted)
            CMPR                  ;PERFORM COMPARISON -- PERFORMS NXT IF FALSE
            JMP     STMT
    S9:     TST     S12,'INPUT'   ;INPUT STATEMENT
    S10:    TSTV    S17           ;GET VAR ADDRESS (Originally CALL VAR = nonexist)
            INNUM                 ;MOVE NUMBER FROM TTY TO AESTK
            STORE                 ;STORE IT
            TST     S11,','       ;IS THERE MORE?
            JMP     S10           ;YES
    S11:    DONE                  ;MUST BE CR
            NXT                   ;SEQUENCE TO NEXT
    S12:    TST     S13,'RETURN'  ;RETURN STATEMENT
            DONE                  ;MUST BE CR
            RSTR                  ;RESTORE LINE NUMBER OF CALL
            NXT                   ;SEQUENCE TO NEXT STATEMENT
    S13:    TST     S14,'END'
            FIN
    S14:    TST     S15,'LIST'    ;LIST COMMAND
            DONE
            LST
            NXT
    S15:    TST     S16,'RUN'     ;RUN COMMAND
            DONE
            NXT
    S16:    TST     S17,'CLEAR'   ;CLEAR COMMAND
            DONE
            JMP     START

    S17:    ERR                   ;SYNTAX ERROR

    EXPR:   TST     E0,'-'
            CALL    TERM          ;TEST FOR UNARY -.
            NEG                   ;GET VALUE
            JMP     E1            ;NEGATE IT
    E0:     TST     E1A,'+'       ;LOOK FOR MORE
    E1A:    CALL    TERM          ;TEST FOR UNARY +
    E1:     TST     E2,'+'        ;LEADING TERM
            CALL    TERM
            ADD
            JMP     E1
    E2:     TST     E3,'-'        ;ANY MORE?
            CALL    TERM          ;DIFFERENCE TERM
            SUB
            JMP     E1
    E3: T2: RTN                   ;ANY MORE?
    TERM:   CALL    FACT
    T0:     TST     T1,'*'
            CALL    FACT          ;PRODUCT FACTOR.
            MUL
            JMP     T0
    T1:     TST     T2,'/'
            CALL    FACT          ;QUOTIENT FACTOR.
            DIV
            JMP     T0

    FACT:   TSTV    F0
            IND                   ;YES, GET THE VALUE.
            RTN
    F0:     TSTN    F1            ;NUMBER, GET ITS VALUE.
            RTN
    F1:     TST     F2,'('        ;PARENTHESIZED EXPR.
            CALL    EXPR
            TST     F2,')'
            RTN
    F2:     ERR                   ;ERROR.

    RELOP:  TST     R0,'='
            LIT     0             ;=
            RTN
    R0:     TST     R4,'<'
            TST     R1,'='
            LIT     2             ;<=
            RTN
    R1:     TST     R3,'>'
            LIT     3             ;<>
            RTN
    R3:     LIT     1             ;<
            RTN
    R4:     TST     S17,'>'
            TST     R5,'='
            LIT     5             ;>=
            RTN
    R5:     TST     R6,'<'
            LIT     3
            RTN                   ;(This line originally omitted)
    R6:     LIT     4
            RTN
