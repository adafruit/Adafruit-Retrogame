EXECS = retrogame menu
CC    = gcc $(CFLAGS) -Wall -O3 -fomit-frame-pointer -funroll-loops -s

all: $(EXECS)

retrogame: retrogame.c
	$(CC) $< -o $@
	strip $@

menu: menu.c
	$(CC) $< -lncurses -lmenu -lexpat -o $@
	strip $@

clean:
	rm -f $(EXECS)
