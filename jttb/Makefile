OBJS=		jttb.o
CLEANFILES=	$(OBJS) jttb
CFLAGS=		-Os -g -Wall -Wformat

all: jttb

jttb.o: Makefile jttb.c ../tbvm/tbvm.h

jttb: $(OBJS) ../tbvm/libtbvm.a
	$(CC) -o $@ $(OBJS) -L ../tbvm -ltbvm -lm

clean:
	-rm -f $(CLEANFILES)
