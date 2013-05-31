EXECS = retrogame
CC    = gcc -Wall -O3 -fomit-frame-pointer -funroll-loops -s

all: $(EXECS)

retrogame: retrogame.c
	$(CC) $< -o $@
	strip $@

clean:
	rm -f $(EXECS)
