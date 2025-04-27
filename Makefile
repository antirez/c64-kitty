all: c64-emu c64-kitty

c64-emu: c64-emu.c
	gcc -O3 c64-emu.c -o c64-emu `sdl2-config --cflags` `sdl2-config --libs` -g -ggdb

c64-kitty: c64-kitty.c
	gcc -O3 c64-kitty.c -o c64-kitty -g -ggdb

clean:
	rm -f c64-emu c64-kitty
