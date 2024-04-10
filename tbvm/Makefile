OBJS=		tbvm.o
CLEANFILES=	$(OBJS) libtbvm.a
CFLAGS=		-O0 -g -Wall -Wformat

all: libtbvm.a

tbvm.o: Makefile tbvm.c tbvm.h tbvm_opcodes.h

libtbvm.a: $(OBJS)
	rm -f $@
	ar -r $@ $(OBJS)
	ranlib $@

clean:
	-rm -f $(CLEANFILES)
