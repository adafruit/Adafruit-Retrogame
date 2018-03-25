#EXECS = retrogame gamera
EXECS = retrogame
CC    = gcc $(CFLAGS) -Wall -Ofast -fomit-frame-pointer -funroll-loops -s

all: $(EXECS)

retrogame: retrogame.c keyTable.h
	$(CC) $< -o $@
	strip $@

ifeq ($(shell [ -r '/usr/include/linux/input-event-codes.h' ] && echo 'X'),X)
KEYFILE = /usr/include/linux/input-event-codes.h
else
KEYFILE = /usr/include/linux/input.h
endif
keyTable.h: keyTableGen.sh $(KEYFILE)
	sh $^ >$@

gamera: gamera.c
	$(CC) $< -lncurses -lmenu -lexpat -o $@
	strip $@

install:
	mv $(EXECS) /usr/local/bin

clean:
	rm -f $(EXECS) keyTable.h
