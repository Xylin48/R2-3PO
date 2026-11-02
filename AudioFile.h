#ifndef R2_AUDIO_FILE_H
#define R2_AUDIO_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* One reader and one lifecycle owner; cancellation is safe on another thread.
 * The owner must join its reader before freeing this decoder. */
typedef struct R2AudioFile R2AudioFile;

R2AudioFile *r2_audio_file_open(const char *path, uint32_t sample_rate,
                                unsigned channels);
ssize_t r2_audio_file_read(R2AudioFile *file, void *buffer, size_t bytes);
void r2_audio_file_cancel(R2AudioFile *file);
void r2_audio_file_close(R2AudioFile *file);

#endif
