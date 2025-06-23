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
#include <sys/time.h>
#include <assert.h>

/* Global configuration (mostly from command line options). */
struct {
    int ghostty_mode;   // Use non standard Kitty protocol that works with
                        // Ghostty, and allows animation, but is incompatible
                        // with Kitty (default).
    int kitty_mode;     // Use graphics protocol with animation codes, this
                        // is needed for the Kitty terminal.
    char *prg_filename; // PRG to execute, if one was given at startup.
} EmuConfig;

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

// run the emulator and render-loop at 30fps
#define FRAME_USEC (33333)

// Function to encode data to base64
size_t base64_encode(const unsigned char *data, size_t input_length, char *encoded_data) {
    const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, j;

    for (i = 0, j = 0; i < input_length; i += 3) {
        uint32_t octet_a = data[i];
        uint32_t octet_b = (i+1 < input_length) ? data[i+1] : 0;
        uint32_t octet_c = (i+2 < input_length) ? data[i+2] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = (i+1 < input_length) ?
                                base64_table[(triple >> 6) & 0x3F] : '=';
        encoded_data[j++] = (i+2 < input_length) ?
                                base64_table[triple & 0x3F] : '=';
    }
    return j;
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
uint8_t *kitty_init(int width, int height, long *kitty_id) {
    // Initialize random seed for image ID
    srand(time(NULL));
    *kitty_id = rand();

    // Allocate framebuffer memory
    uint8_t *fb = malloc(width * height * 3);
    memset(fb, 0, width * height * 3);
    return fb;
}

// Update display using Kitty graphics protocol
void kitty_update_display(long kitty_id, int frame_number, int width, int height, uint8_t *fb) {
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

    // Send Kitty Graphics Protocol escape sequence with base64 data.
    // Kitty allows a maximum chunk of 4096 bytes each.
    size_t encoded_offset = 0;
    size_t chunk_size = 4096;
    while(encoded_offset < encoded_size) {
        int more_chunks = (encoded_offset + chunk_size) < encoded_size;
        if (encoded_offset == 0) {
            if (EmuConfig.ghostty_mode) {
                printf("\033_Ga=%c,i=%lu,f=24,s=%d,v=%d,q=2,c=30,r=10,m=%d;",
                    frame_number == 0 ? 'T' : 't',  kitty_id, width, height,
                    more_chunks);
            } else {
                if (frame_number == 0) {
                    printf("\033_Ga=T,i=%lu,f=24,s=%d,v=%d,q=2,"
                           "c=30,r=10,m=%d;",
                        kitty_id, width, height, more_chunks);
                } else {
                    printf("\033_Ga=f,r=1,i=%lu,f=24,x=0,y=0,s=%d,v=%d,m=%d;",
                        kitty_id, width, height, more_chunks);
                }
            }
        } else {
            if (EmuConfig.ghostty_mode) {
                printf("\033_Gm=%d;", more_chunks);
            } else {
                // Chunks after the first just require the raw data and the
                // more flag.
                if (frame_number == 0) {
                    printf("\033_Gm=%d;", more_chunks);
                } else {
                    printf("\033_Ga=f,r=1,m=%d;", more_chunks);
                }
            }
        }

        // Transfer payload.
        size_t this_size = more_chunks ? 4096 : encoded_size-encoded_offset;
        fwrite(encoded_data+encoded_offset, this_size, 1, stdout);
        printf("\033\\");
        fflush(stdout);
        encoded_offset += this_size;
    }

    if (EmuConfig.kitty_mode && frame_number > 0) {
        // In Kitty mode we need to emit the "a" action to update
        // our area with the new frame.
        printf("\033_Ga=a,c=1,i=%lu;", kitty_id);
        printf("\033\\");
    }

    /* When the image is created, add a newline so that the cursor
     * is more naturally placed under the image, not at the right/bottom
     * corner. */
    if (frame_number == 0) {
        printf("\r\n");
        fflush(stdout);
    }

    // Clean up
    free(encoded_data);
}

// Process keyboard input, sets the pressed or released key into the
// state of the emulator. Returns 0 for any key, and 1 if the user
// requested to stop the emulator.
int process_keyboard(c64_t *c64) {
    int bytes_waiting = kbhit();
    char ch[8];

    if (!bytes_waiting) return 0; // No keyboard events pending.
    for (int j = 0; j < bytes_waiting && j < (int)sizeof(ch); j++)
        ch[j] = getchar();

    int c64_key = 0;

    if (ch[0] == 27 && bytes_waiting == 1) { // Just ESC.
        return 1;
    } else if (bytes_waiting == 3 && ch[0] == 27 && ch[1] == '[') {
        switch(ch[2]) {
        case 'A': c64_key = C64_KEY_CSRUP; break;
        case 'B': c64_key = C64_KEY_CSRDOWN; break;
        case 'C': c64_key = C64_KEY_CSRRIGHT; break;
        case 'D': c64_key = C64_KEY_CSRLEFT; break;
        default:
            printf("Not handled escape: ESC[%c\r\n", ch[2]);
            break;
        }
    } else {
        c64_key = ch[0];
        if (islower(c64_key)) c64_key = toupper(c64_key);
        else if (isupper(c64_key)) c64_key = tolower(c64_key);
        else if (c64_key == 127 || c64_key == 8) c64_key = C64_KEY_DEL;
    }

    if (c64_key == 0) return 0;

    // Map key to C64 keycode and send it to the emulator.
    c64_key_down(c64, c64_key);
    c64_key_up(c64, c64_key);
    return 0;
}

void crt_set_pixel(void *fbptr, int x, int y, uint32_t color) {
    uint8_t *fb = fbptr;

    if (x < 0 || x >= _C64_SCREEN_WIDTH || y < 0 || y >= _C64_SCREEN_HEIGHT)
        return;

    uint8_t *dst = fb + (x*3+y*_C64_SCREEN_WIDTH*3);
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
void audio_cleanup(void *user_data);
#endif

/* Initialize and parse the configuration, storing it into the
 * global EmuConfig structure. */
void parse_config(int argc, char **argv) {
    EmuConfig.ghostty_mode = 1;
    EmuConfig.kitty_mode = 0;
    EmuConfig.prg_filename = NULL;

    for (int j = 1; j < argc; j++) {
        if (!strcasecmp(argv[j],"--kitty")) {
            EmuConfig.kitty_mode = 1;
            EmuConfig.ghostty_mode = 0;
        } else if (!strcasecmp(argv[j],"--ghostty")) {
            EmuConfig.kitty_mode = 0;
            EmuConfig.ghostty_mode = 1;
        } else {
            if (argv[j][0] != '-' && EmuConfig.prg_filename == NULL) {
                EmuConfig.prg_filename = strdup(argv[j]);
            } else {
                fprintf(stderr, "Unrecognized option: %s\n", argv[j]);
                exit(1);
            }
        }
    }
}

int main(int argc, char **argv) {
    c64_t c64;
    c64_desc_t c64_desc = {0};

    parse_config(argc, argv);

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

    /* Initialize Kitty graphics */
    int width  = _C64_SCREEN_WIDTH;
    int height = _C64_SCREEN_HEIGHT;
    long kitty_id;
    uint8_t *fb = kitty_init(width, height, &kitty_id);

    /* C64 emulator init. */
    c64_desc.roms.chars.ptr = dump_c64_char_bin;
    c64_desc.roms.chars.size = sizeof(dump_c64_char_bin);
    c64_desc.roms.basic.ptr = dump_c64_basic_bin;
    c64_desc.roms.basic.size = sizeof(dump_c64_basic_bin);
    c64_desc.roms.kernal.ptr = dump_c64_kernalv3_bin;
    c64_desc.roms.kernal.size = sizeof(dump_c64_kernalv3_bin);
    c64_desc.crt_set_pixel = crt_set_pixel;
    c64_desc.crt_set_pixel_fb = fb;
    c64_init(&c64, &c64_desc);

    /* Get C64 display information */
    chips_display_info_t di = c64_display_info(&c64);
    printf("FB total size %dx%d\n", di.frame.dim.width, di.frame.dim.height);
    printf("FB screen %dx%d at %dx%d\n", di.screen.width, di.screen.height, di.screen.x, di.screen.y);

    printf("C64 Emulator started. Press 'ESC' to quit.\n");

    // Enable raw mode for keyboard input
    enable_raw_mode();

    // run the emulation/input/render loop
    int frame = 0;
    uint64_t total_us_emulated = 0;
    uint64_t total_us_start = time_us();
    int quit_requested = 0;

    while (!quit_requested) {
        // tick the emulator for 1 frame
        c64_exec(&c64, FRAME_USEC);
        total_us_emulated += FRAME_USEC;

        // Handle keyboard input
        quit_requested = process_keyboard(&c64);

        // Update display using Kitty protocol
        kitty_update_display(kitty_id, frame++, width, height, fb);

        // Synchronize the emulated C64 at its theoretical speed.
        uint64_t total_us_real = time_us() - total_us_start;
        int64_t delta_us = total_us_emulated - total_us_real;
        int64_t to_sleep_us = FRAME_USEC + delta_us;
        if (to_sleep_us > 0) usleep(to_sleep_us);

        // Load the C64 provided PRG file if any.
        if (frame == 90 && EmuConfig.prg_filename) {
            load_prg_file(&c64,EmuConfig.prg_filename);
        }
    }

#ifdef USE_AUDIO
    audio_cleanup(audio_user_data);
#endif
    // Cleanup
    free(fb);
    disable_raw_mode();
    printf("\nC64 Emulator terminated.\n");

    return 0;
}
