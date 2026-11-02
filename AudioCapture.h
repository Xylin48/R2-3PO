#ifndef R2_AUDIO_CAPTURE_H
#define R2_AUDIO_CAPTURE_H

#include <stddef.h>
#include <sys/types.h>
#include <pulse/sample.h>

/* Exactly one reader and one lifecycle owner per capture. Cancel is safe
 * from a different thread, including during a pending connect or read. */
typedef struct R2AudioCapture R2AudioCapture;

R2AudioCapture *r2_audio_capture_new(void);
int r2_audio_capture_connect(R2AudioCapture *capture, const char *source,
                             const pa_sample_spec *format, size_t fragment_bytes);
ssize_t r2_audio_capture_read(R2AudioCapture *capture, void *buffer, size_t bytes);
void r2_audio_capture_cancel(R2AudioCapture *capture);
/* Only call free once the capture reader has exited. */
void r2_audio_capture_free(R2AudioCapture *capture);

#endif
