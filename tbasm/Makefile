OBJS=		tbasm.o
CLEANFILES=	$(OBJS) tbasm
CFLAGS=		-O0 -g -Wall -Wformat

all: tbasm

tbasm: $(OBJS) ../tbvm/tbvm_opcodes.h
	cc -o tbasm $(OBJS)

clean:
	-rm -f $(CLEANFILES)
