OBJS=		tbvm.o
CLEANFILES=	$(OBJS) tbvm_program.h libtbvm.a
CFLAGS=		-Os -g -Wall -Wformat

all: libtbvm.a

tbvm.o: Makefile tbvm.c tbvm.h tbvm_opcodes.h tbvm_program.h

tbvm_program.h: tbvm_program.asm
	../tbasm/tbasm -H $<

libtbvm.a: $(OBJS)
	rm -f $@
	ar -r $@ $(OBJS)
	ranlib $@

clean:
	-rm -f $(CLEANFILES)
