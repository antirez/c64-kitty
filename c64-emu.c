/*
    c64.c

    Stripped down C64 emulator running in a (xterm-256color) terminal.
*/
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <SDL.h>

#define CHIPS_IMPL
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "beeper.h"
#include "kbd.h"
#include "mem.h"
#include "clk.h"
#include "c1530.h"
#include "m6522.h"
#include "c1541.h"
#include "c64.h"
#include "c64-roms.h"

/* SDL initialization function. */
static SDL_Texture *sdlInit(int width, int height, int fullscreen, SDL_Renderer **rp) {
    int flags = SDL_WINDOW_OPENGL;
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (SDL_Init(SDL_INIT_VIDEO) == -1) {
        fprintf(stderr, "SDL Init error: %s\n", SDL_GetError());
        return NULL;
    }
    atexit(SDL_Quit);
    screen = SDL_CreateWindow("C64",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              width,height,flags);
    if (!screen) {
        fprintf(stderr, "Can't create SDL window: %s\n", SDL_GetError());
        return NULL;
    }

    renderer = SDL_CreateRenderer(screen,-1,0);
    if (!renderer) {
        fprintf(stderr, "Can't create SDL renderer: %s\n", SDL_GetError());
        return NULL;
    }

    texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING,
                                width,height);
    if (!texture) {
        fprintf(stderr, "Can't create SDL texture: %s\n", SDL_GetError());
        return NULL;
    }
    *rp = renderer;
    return texture;
}

/* Show a raw RGB image on the SDL screen. */
static void sdlShowRgb(SDL_Texture *texture, SDL_Renderer *renderer, unsigned char *fb, int width,
        int height)
{
    (void)height;
    SDL_UpdateTexture(texture,NULL,fb,width*3);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* Minimal SDL event processing, just a few keys to exit the program.
 *
 * WARNING: With OSX SDL port, if you don't process events, no window
 * will show at all :D :DD :DDD. */
static void sdlProcessEvents(void) {
    SDL_Event event;

    while(SDL_PollEvent(&event)) {
        switch(event.type) {
        case SDL_KEYDOWN:
            switch(event.key.keysym.sym) {
            case SDLK_q:
            case SDLK_ESCAPE:
                exit(0);
                break;
            default: break;
            }
        }
    }
}

static c64_t c64;

// run the emulator and render-loop at 30fps
#define FRAME_USEC (33333)
// border size
#define BORDER_HORI (5)
#define BORDER_VERT (3)

// a signal handler for Ctrl-C, for proper cleanup
static int quit_requested = 0;
static void catch_sigint(int signo) {
    (void)signo;
    quit_requested = 1;
}

// conversion table from C64 font index to ASCII (the 'x' is actually the pound sign)
static char font_map[65] = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[x]   !\"#$%&`()*+,-./0123456789:;<=>?";

// map C64 color numbers to xterm-256color colors
static int colors[16] = {
    16,     // black
    231,    // white
    88,     // red
    73,     // cyan
    54,     // purple
    71,     // green
    18,     // blue
    185,    // yellow
    136,    // orange
    58,     // brown
    131,    // light-red
    59,     // dark-grey
    102,    // grey
    150,    // light green
    62,     // light blue
    145,    // light grey
};

uint8_t *sdlfb;
int c64width, c64height;

void crt_set_pixel(int x, int y, uint32_t color) {
    uint8_t *dst = sdlfb + (x*3+y*c64width*3);
    dst[0] = color & 0xff;
    dst[1] = (color>>8) & 0xff;
    dst[2] = (color>>16) & 0xff;
}

int main(int argc, char* argv[]) {
    //printf("%d\n",(int)sizeof(c64)); exit(1);

    /* C64 emulator init. */
    (void)argc; (void)argv;
    c64_desc_t c64_desc = {0};
    c64_desc.roms.chars.ptr = dump_c64_char_bin;
    c64_desc.roms.chars.size = sizeof(dump_c64_char_bin);
    c64_desc.roms.basic.ptr = dump_c64_basic_bin;
    c64_desc.roms.basic.size = sizeof(dump_c64_basic_bin);
    c64_desc.roms.kernal.ptr = dump_c64_kernalv3_bin;
    c64_desc.roms.kernal.size = sizeof(dump_c64_kernalv3_bin);
    c64_desc.crt_set_pixel = crt_set_pixel;
    c64_init(&c64, &c64_desc);

    // install a Ctrl-C signal handler
    signal(SIGINT, catch_sigint);

    /* SDL Init. */
    chips_display_info_t di = c64_display_info(&c64);
    int fbsize = di.frame.buffer.size;
    uint32_t *palette = di.palette.ptr;
    printf("FB total size %dx%d\n",di.frame.dim.width, di.frame.dim.height);
    printf("FB screen %dx%d at %dx%d\n",di.screen.width, di.screen.height, di.screen.x, di.screen.y);

    int width = di.frame.dim.width;
    int height = di.frame.dim.height;

    SDL_Texture *texture;
    SDL_Renderer *renderer;
    texture = sdlInit(width,height,0,&renderer);
    c64width = width;
    c64height = height;
    sdlfb = malloc(width*height*3);

    // run the emulation/input/render loop
    while (!quit_requested) {
        // tick the emulator for 1 frame
        c64_exec(&c64, FRAME_USEC);

        // Keys handling
        /*
        c64_key_down(&c64, ch);
        c64_key_up(&c64, ch);
        */

        // Update SDL screen
        sdlShowRgb(texture, renderer, sdlfb, width, height);
        sdlProcessEvents();

        // pause until next frame
        usleep(FRAME_USEC);
        printf("."); fflush(stdout);
    }
    return 0;
}
