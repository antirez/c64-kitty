#include <stdio.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

void *audio_init(void)
{
	int error = 0;

	pa_simple *pa_connection = NULL;

	pa_sample_spec sample_format = {
		.format = PA_SAMPLE_FLOAT32,
		.rate = 44100,
		.channels = 1
	};

	/*
	 * pa_connection holds the connection to the pulseaudio server
	 * we're using the simple API provided by Pulseaudio, as referred
	 * in:
	 *    https://freedesktop.org/software/pulseaudio/doxygen/simple.html
	 */
	pa_connection = pa_simple_new(
			NULL,				// Use the default server.
			"c64 Emulator Kitty",
			PA_STREAM_PLAYBACK,
			NULL,				// Use the default device.
			"c64 Stream",
			&sample_format,
			NULL,				// Use default channel map
			NULL,				// Use default buffering attributes.
			&error);
	if (!pa_connection) {
		return NULL;
	}

	return (void *)pa_connection;
}

/* This function receive samples from the emulator. It will
 * simply reproduce the samles received using pa_simple_write()
 *     https://freedesktop.org/software/pulseaudio/doxygen/simple_8h.htm
 */
void audio_from_emulator(const float *samples, int num_samples, void *user_data)
{
	int error = 0;
	pa_simple *pa_connection = user_data;

	error = pa_simple_write(
				pa_connection,
				samples,
				num_samples * sizeof(float),
				&error);

	if (error < 0)
		fprintf(stderr,
				"Failed to write data to audio server: %s\n",
				pa_strerror(error));
}

void audio_cleanup(void *user_data)
{
	pa_simple *pa_connection = user_data;
	pa_simple_free(pa_connection);
}
