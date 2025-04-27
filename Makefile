all: c64-kitty

c64-kitty: c64-kitty.c
	gcc -O3 c64-kitty.c -o c64-kitty -g -ggdb

clean:
	rm -f c64-kitty
