# JTTB - Jason's Tiny BASIC

This is a small BASIC interpreter that started out as a straight-forward
implementation of the original Tiny BASIC "[specification](http://www.ittybittycomputers.com/IttyBitty/TinyBasic/DDJ1/Design.html)" using the
Tiny BASIC virtual machine, inspired by a mild case of boredom while I
was on a plane flight.  Much to my delight, my boredom was cured and it's
now a fun little plaything where I can channel my nostalgia.

This dialect of Tiny BASIC is written in very portable C, and leverages
the C run-time to do some of the heavy lifting (memory management,
specifically).  And while the code is reasonably small, it doens't go to
any great effort to squeeze out every last byte of memory efficiency possible;
The program store, for example, is a sparsely populated fixed-size array
of pointers.

Jason's Tiny BASIC also takes some syntactic inspiration from the classic
Microsoft BASIC of the late 70s and early 80s, specifically the TRS-80
Extended Color BASIC variant, since that's what I grew up on.  This includes
the PRINT and FOR loop syntax, as well as several of the built-in functions
found in that (and other) flavors of Microsoft BASIC.

Jason's Tiny BASIC also supports string variables (*A$ ... Z$*) and
treats strings as first-class objects, unlike the original Tiny BASIC
which only supported immediate (static) strings and only for printing.
Strings can be included in expressions (and adding two strings with
the *+* operator creates a new string object whose value is the
concatenation of the two strings); type mis-matches are detected at
run-time.

Staying true to the original Tiny BASIC specification, Jason's Tiny BASIC
is built using a virtual machine, and the virtual machine is capable of
executing the original example Tiny BASIC VM program published in the
original article (well, with the well-known minor corrections, anyway).

Above all, Jason's Tiny BASIC is a work-in-progress, is not at all meant
to be serious software, and is purely a vehicle for my own amusement.  So
just keep that in mind when you're looking at the stuff that's here! :-)

## Components of Jason's Tiny BASIC

There are 3 main components of Jason's Tiny BASIC:

* The JTTB Tiny BASIC virtual machine, *tbvm*, which implements the program
store, and the other supporting functions, as well as implements the VM opcodes
of the Tiny BASIC VM assembly language that the heart and soul of the
interpreter is written in.
* An assembler for the JTTB Tiny BASIC virtual machine assembly language,
*tbasm*, the output of which is executed by the Tiny BASIC virtual machine.
* The driver program, *jttb*, which includes the Tiny BASIC virtual machine
program, and a small wrapper around the *tbvm* library.

There were a couple of ideas behind separating things out this way:
* By making the virtual machine a library, it would be relatively
easy to provide additional inputs/outputs to the virtual machine that
would enable embedding Jason's Tiny BASIC as an extension language
into something else, if anyone were to be crazy enough to do such a
silly thing.
* By separating the VM program from the back-end, it provides a simple
mechanism to play alternate syntaxes or even translating Jason's Tiny
BASIC into something not based on English.

### The JTTB Tiny BASIC Virtual Machine

At its core, the JTTB VM is relatively simple.  It is structured around
three stacks:

* [The control stack](docs/stacks.md#-the-control-stack)
* [The subroutine stack](docs/stacks.md#-the-subroutine-stack)
* [The expression stack](docs/stacks.md#-the-expression-stack)

The control stack tracks return addresses for subroutines within the
VM program itself.

The subroutine stack tracks return locations for subroutines within the
BASIC program, used to support the **GOSUB** statement.  It also stores
the state needed for **FOR** loops.  For each loop, it records the following:

* The variable involved in the loop
* The line number of the first statement in the loop body
* The starting value to assign to the variable
* The terminating value of the variable
* The loop stepping (the value to add to the variable each time through
the loop)

The expression stack is used to hold:

* The left and right values used with arithmetic operators
* Arguments to functions
* Results of arithmetic operations and functions

The expression stack is an integral part of the typing system in JTTB.

In addition to these four stacks, there are a few other supporting
characters that support the core functionality of the BASIC language.
Those are:

* The variable store
* The program store
* The line buffer
* The line cursor
* The string store

And finally, there are a few other odds and ends:

* I/O routines
* Exception handling (BASIC errors, VM internal errors)
* VM program state

The *tbvm_exec()* function implements the VM's main execution loop.  When
called, it first extracts two special VM program addresses from the VM
program, the *collector PC* and the *executor PC*.  It then initializes
everything, including two *setjmp()* environments for BASIC errors and
VM internal errors.  Finally, it enters the main loop which performs the
following tasks in order:

* Garbage-collects unreferenced strings
* Checks for a BREAK from the console
* Fetches the next opcode from the VM program
* Calls the function that implements that opcode

If a VM internal error occurs (signalled by the *vm_abort()* function),
the *longjmp()* target marks the VM as no longer running and *tbvm_exec()*
returns.

If a BASIC error occurs (signalled by the *basic_error()* function),
the *longjmp()* target forces the interpreter back into **direct** mode
and re-enters the main loop.

(I will write some more notes here about how it works, eventually.  Promise!)

### The JTTB Tiny BASIC Virual Machine Assembler

(I will write some notes here about how it works, eventually.  Promise!)

### The JTTB Driver Program

(I will write some notes here about how it works, eventually.  Promise!)

## How to build it

(I will write some notes here about how to built it, eventually.  Promise!)

## The Stuff At The Bottom Of The Page

If you have any questions or feedback, I'd love to hear it!  You can reach
out to me on Twitter (*[@thorpej](https://twitter.com/thorpej)*) or Mastodon
(*[@thorpej@mastodon.sdf.org](https://mastodon.sdf.org/@thorpej)*).  You
can also check out my [YouTube channel](https://www.youtube.com/@thorpejsf),
which has this and other retrocomputing related content.
