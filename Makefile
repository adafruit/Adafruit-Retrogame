#EXECS  = retrogame gamera
EXECS  = retrogame
CFLAGS = -Wall -Ofast -fomit-frame-pointer -funroll-loops -s
LIBS   = -lgpiod
CC     = gcc $(CFLAGS)

all: $(EXECS)

retrogame: retrogame.c keyTable.h
	$(CC) $< $(LIBS) -o $@
	strip $@

# KEYFILE = /usr/include/linux/input.h
KEYFILE = /usr/include/linux/input-event-codes.h
keyTable.h: keyTableGen.sh $(KEYFILE)
	sh $^ >$@

gamera: gamera.c
	$(CC) $< -lncurses -lmenu -lexpat -o $@
	strip $@

install:
	mv $(EXECS) /usr/local/bin

clean:
	rm -f $(EXECS) keyTable.h
