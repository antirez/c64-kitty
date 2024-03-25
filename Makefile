all: c64-emu

c64-emu: c64-emu.c
	gcc -O3 c64-emu.c -o c64-emu `sdl2-config --cflags` `sdl2-config --libs`

clean:
	rm -f c64-emu
