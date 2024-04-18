# The Control Stack

The control stack's function is very straight-forward: it is a 64-entry
array of VM program counter addresses that stores the return address of
VM program subroutine calls.  It is primarily used by the *CALL* and
*RTN* VM instructions.

The control stack has one additional feature that is helpful in implementing
**IF** / **THEN** / **ELSE** blocks.  The *ONDONE* VM instruction sets a VM
address to be *CALL*'d when the VM program executes a *DONE* instruction.
The address of the *DONE* instruction is pushed onto the control stack and
when the *ondone* hooks returns (using *RTN*), the *DONE* instruction
is executed again, only this time the *ondone* hook will not be set.  Only
one *ondone* hook can be set at a time; attempting to use *ONDONE* while an
*ondone* hook is already set will result in a syntax error.

# The Subroutine Stack

The subroutine stack is similar to the control stack in that it stores
return addresses, but for the BASIC program.  This is used to implement
the **GOSUB** statement.  It also stores the context needed to support
**FOR** / **NEXT** loops.  The subroutine stack is a 90-entry array, and
can also unwind multiple entries at a time.  Consider the following:

    10 FOR I=1 TO 10
    20 FOR J=101 TO 201
    30 PRINT I,J
    40 IF J % 10 = 0 THEN NEXT I ELSE NEXT J
    50 END

Whenever the predicate on line 40 is true, we pop the J loop off the stack
completely before its loop would naturally terminate because instead we
run the next iteration of the next iteration of the I loop.

The following example demonstrates why tracking loops with the subroutine stack is convenient:

     10 FOR I=1 TO 10
     20 PRINT "BEFORE GOSUB", I
     30 GOSUB 100
     40 PRINT "AFTER GOSUB", I
     50 NEXT I
     60 END
    100 IF I=5 THEN NEXT I
    110 RETURN

In this example, if the predicate on line 100 is true, the "NEXT I" needs
to also pop the "GOSUB 100".  The correct output for this program
is:

    BEFORE GOSUB 1
    AFTER GOSUB 1
    BEFORE GOSUB 2
    AFTER GOSUB 2
    BEFORE GOSUB 3
    AFTER GOSUB 3
    BEFORE GOSUB 4
    AFTER GOSUB 4
    BEFORE GOSUB 5
    BEFORE GOSUB 6
    AFTER GOSUB 6
    BEFORE GOSUB 7
    AFTER GOSUB 7
    BEFORE GOSUB 8
    AFTER GOSUB 8
    BEFORE GOSUB 9
    AFTER GOSUB 9
    BEFORE GOSUB 10
    AFTER GOSUB 10

# The Expression Stack

The expression stack is really the nexus of all activity in the JTTB BASIC
interpreter, and is at the core of the typing system used in JTTB.
In addition to arithmetic expression evaluation, the expression stack
is used to pass arguments to functions, which in turn return their
results on the expression stack.  Nearly all BASIC statements in JTTB
use expressions, including **GOTO** and **GOSUB**.  Note that the
expression evaluator itself is actually implemented in the VM program,
but it relies on the expression stack to do all of its work.

The expression stack is a 64-entry array of *values*.  A *value* may be
an integer, a string, or a variable reference.  When values are popped
off the stack, the expected type can be specified, or, if the operation
supports multiple value types, the "any" type can be used.  If the wrong
type of value is popped, a **?WRONG VALUE TYPE** BASIC error occurs and
program execution is stopped.  If the expression stack overflows, an
**?EXPRESSION TOO COMPLEX** BASIC error occurs.