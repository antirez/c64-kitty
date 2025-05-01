all: c64-kitty

c64-kitty: c64-kitty.c
	gcc -O2 -Wall -W c64-kitty.c -o c64-kitty -g -ggdb

macos-audio: c64-kitty.c audio_macos.c
	gcc -D USE_AUDIO -O2 -Wall -W c64-kitty.c audio_macos.c -o c64-kitty -g -ggdb -framework AudioToolbox -framework CoreFoundation

linux-pulseaudio: c64-kitty.c audio_linux.c
	gcc -D USE_AUDIO -O2 -Wall -W -lpulse -lpulse-simple c64-kitty.c audio_linux.c -o c64-kitty -g -ggdb

clean:
	rm -f c64-kitty
