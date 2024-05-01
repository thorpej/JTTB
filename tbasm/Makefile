OBJS=		tbasm.o
CLEANFILES=	$(OBJS) tbasm
CFLAGS=		-Os -g -Wall -Wformat

all: tbasm

tbasm: $(OBJS) ../tbvm/tbvm_opcodes.h
	cc -o tbasm $(OBJS)

clean:
	-rm -f $(CLEANFILES)
