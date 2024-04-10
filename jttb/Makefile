OBJS=		jttb.o
CLEANFILES=	$(OBJS) jttb jttb_vmprog.h
CFLAGS=		-O0 -g -Wall -Wformat

#VMASM=		tinybasic_orig.asm
VMASM=		jttb.asm

all: jttb

jttb.o: Makefile jttb.c jttb_vmprog.h ../tbvm/tbvm.h

jttb: $(OBJS) ../tbvm/libtbvm.a
	$(CC) -o $@ $(OBJS) -L ../tbvm -ltbvm

jttb_vmprog.h: $(VMASM)
	../tbasm/tbasm -Hjttb_vmprog.h $<

clean:
	-rm -f $(CLEANFILES)
