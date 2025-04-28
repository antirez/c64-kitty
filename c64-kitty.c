/* c64-kitty.c
 * C64 running in a terminal using Kitty Graphics Protocol.  */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <assert.h>

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

static c64_t c64;
static int quit_requested = 0;
uint8_t *fb;
int c64width, c64height;
long kitty_id;

// run the emulator and render-loop at 30fps
#define FRAME_USEC (33333)

// Base64 encoding table
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Function to encode data to base64
size_t base64_encode(const unsigned char *data, size_t input_length, char *encoded_data) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    size_t i, j;

    for (i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = base64_table[triple & 0x3F];
    }

    // Add padding if needed
    size_t mod_table[] = {0, 2, 1};
    for (i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[output_length - 1 - i] = '=';

    return output_length;
}

// Terminal keyboard input handling
struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int kbhit() {
    int bytesWaiting;
    ioctl(STDIN_FILENO, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}

// Initialize Kitty graphics protocol
void kitty_init(int width, int height) {
    // Initialize random seed for image ID
    srand(time(NULL));
    kitty_id = rand();

    // Allocate framebuffer memory
    fb = malloc(width * height * 3);
    memset(fb, 0, width * height * 3);
}

// Update display using Kitty graphics protocol
void kitty_update_display(int frame, int width, int height) {
    // Calculate base64 encoded size
    size_t bitmap_size = width * height * 3;
    size_t encoded_size = 4 * ((bitmap_size + 2) / 3);
    char *encoded_data = (char*)malloc(encoded_size + 1);

    if (!encoded_data) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    // Encode the bitmap data to base64
    base64_encode(fb, bitmap_size, encoded_data);
    encoded_data[encoded_size] = '\0';  // Null-terminate the string

    // Send Kitty Graphics Protocol escape sequence with base64 data
    printf("\033_Ga=%c,i=%lu,f=24,s=%d,v=%d,q=2,c=30,r=10;", frame == 0 ? 'T' : 't',  kitty_id, width, height);
    printf("%s", encoded_data);
    printf("\033\\");
    if (frame == 0) printf("\r\n");
    fflush(stdout);

    // Clean up
    free(encoded_data);
}

// Process keyboard input
void process_keyboard() {
    if (kbhit()) {
        char ch = getchar();
        if (ch == 'q' || ch == 'Q' || ch == 27) { // q, Q or ESC
            quit_requested = 1;
            return;
        }

        // Map key to C64 keycode and send it to the emulator
        c64_key_down(&c64, ch);
        c64_key_up(&c64, ch);
    }
}

void crt_set_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= c64width || y < 0 || y >= c64height) return;

    uint8_t *dst = fb + (x*3+y*c64width*3);
    dst[0] = color & 0xff;         // R
    dst[1] = (color>>8) & 0xff;    // G
    dst[2] = (color>>16) & 0xff;   // B
}

uint64_t time_us(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    uint64_t usec = (uint64_t)(tv.tv_sec) * 1000000 +
	            (uint64_t)(tv.tv_usec);
    return usec;
}

/* Load a PRG file in the C64 RAM. */
int load_prg_file(c64_t* sys, const char *filename) {
    uint8_t *buffer = NULL;
    size_t file_size = 0;
    int success = 0;

    FILE *file = fopen(filename, "rb");
    if (!file) {
	perror("Error opening file");
	fprintf(stderr, "Failed to open PRG file: %s\n", filename);
	goto cleanup;
    }

    // Check file length in a portable way.
    if (fseek(file, 0, SEEK_END) != 0) {
        perror("Error seeking to end of file");
        fprintf(stderr, "Failed to determine size of PRG file: %s\n", filename);
        goto cleanup;
    }
    file_size = ftell(file);

    // Load file content.
    fseek(file, 0, SEEK_SET);
    buffer = (uint8_t*)malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate memory (%d bytes) for PRG file: %s\n", (int)file_size, filename); goto cleanup;
    }

    size_t bytes_read = fread(buffer, 1, file_size, file);
    assert (bytes_read == file_size);

    // Prepare data structure for c64_quickload() function and load.
    chips_range_t prg_data;
    prg_data.ptr = buffer;
    prg_data.size = file_size;

    if (c64_quickload(sys, prg_data)) {
        printf("Successfully loaded PRG file via c64_quickload: %s (%d bytes)\n\r", filename, (int)file_size);
        success = 1;
        int start_addr = buffer[1]<<8 | buffer[0];
        printf("Run the program with SYS %d\r\n", start_addr);
        // c64_basic_run(sys);
    } else {
        fprintf(stderr, "Error: c64_quickload function failed for file: %s\n\r", filename);
        success = 0;
    }

cleanup:
    if (file) fclose(file);
    if (buffer) free(buffer);
    return success;
}

#ifdef USE_AUDIO
void *audio_init(void);
void audio_from_emulator(const float *samples, int num_samples, void *user_data);
#endif

int main(int argc, char* argv[]) {
    c64_desc_t c64_desc = {0};

    /* Initialize the audio subsystem. */
#ifdef USE_AUDIO
    void *audio_user_data = audio_init();
    if (audio_user_data == NULL) {
        fprintf(stderr,"Audio initialization failed\n");
        exit(1);
    }
    chips_audio_callback_t audio_cb;
    audio_cb.func = audio_from_emulator;
    audio_cb.user_data = audio_user_data;
    c64_desc.audio.callback = audio_cb;
#endif

    /* C64 emulator init. */
    c64_desc.roms.chars.ptr = dump_c64_char_bin;
    c64_desc.roms.chars.size = sizeof(dump_c64_char_bin);
    c64_desc.roms.basic.ptr = dump_c64_basic_bin;
    c64_desc.roms.basic.size = sizeof(dump_c64_basic_bin);
    c64_desc.roms.kernal.ptr = dump_c64_kernalv3_bin;
    c64_desc.roms.kernal.size = sizeof(dump_c64_kernalv3_bin);
    c64_desc.crt_set_pixel = crt_set_pixel;
    c64_init(&c64, &c64_desc);

    /* Get C64 display information */
    chips_display_info_t di = c64_display_info(&c64);
    printf("FB total size %dx%d\n", di.frame.dim.width, di.frame.dim.height);
    printf("FB screen %dx%d at %dx%d\n", di.screen.width, di.screen.height, di.screen.x, di.screen.y);

    int width = di.screen.width;
    int height = di.screen.height;
    c64width = width;
    c64height = height;

    /* Initialize Kitty graphics */
    kitty_init(width, height);

    printf("C64 Emulator started. Press 'q' or 'ESC' to quit.\n");

    // Enable raw mode for keyboard input
    enable_raw_mode();

    // run the emulation/input/render loop
    int frame = 0;
    uint64_t total_us_emulated = 0;
    uint64_t total_us_start = time_us();
    while (!quit_requested) {
        // tick the emulator for 1 frame
        uint64_t start_usec = time_us();
        c64_exec(&c64, FRAME_USEC);
        total_us_emulated += FRAME_USEC;

        // Handle keyboard input
        process_keyboard();

        // Update display using Kitty protocol
        kitty_update_display(frame++, width, height);
        uint64_t elapsed_usec = time_us() - start_usec;

        // pause until next frame
        if (elapsed_usec < FRAME_USEC) {
            /* The emulated C64 may run at a slightly different speed than
             * what we want, so we don't just wait for the theoretical
             * frame time, but also adjust for the loop time spent in
             * refreshing the screen, plus some global accounting to make
             * sure we are in sync. */
            uint64_t total_us_real = time_us() - total_us_start;
            uint64_t delta_us = total_us_emulated - total_us_real;
            int64_t to_sleep_us = FRAME_USEC - elapsed_usec + delta_us;
            if (to_sleep_us > 0) usleep(to_sleep_us);
        }

        /* Load the C64 provided PRG file if any. */
        if (frame == 90 && argc >= 2) {
            load_prg_file(&c64,argv[1]);
        }
    }

    // Cleanup
    free(fb);
    disable_raw_mode();
    printf("\nC64 Emulator terminated.\n");

    return 0;
}
