#ifndef R2_INPUT_H
#define R2_INPUT_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
    R2_INPUT_LINE_IO_ERROR = -3,
    R2_INPUT_LINE_EMBEDDED_NUL = -2,
    R2_INPUT_LINE_TOO_LONG = -1,
    R2_INPUT_LINE_EOF = 0,
    R2_INPUT_LINE_OK = 1
} R2InputLineStatus;

/* Leave room for the message role and framing in the 32 MiB live buffer. */
#define R2_MAX_TURN_INPUT_BYTES (32U * 1024U * 1024U - 64U)

/* Grow on demand while retaining a hard storage bound. Drain an overlong
 * line without dispatching a prefix. LF and CRLF are terminal framing. */
static inline R2InputLineStatus
r2_read_complete_line(FILE *stream, char **buffer, size_t *capacity,
                      size_t max_bytes, size_t *out_length)
{
    if (!stream || !buffer || !capacity || !out_length ||
        (!*buffer && *capacity) || max_bytes > SIZE_MAX - 2) {
        errno = EINVAL;
        return R2_INPUT_LINE_IO_ERROR;
    }
    const size_t maximum_storage = max_bytes + 2;
    size_t used = 0;
    int too_long = 0;
    int embedded_nul = 0;
    int ch;
    while ((ch = fgetc(stream)) != EOF && ch != '\n') {
        if (ch == '\0') embedded_nul = 1;
        if (used == maximum_storage - 1) {
            too_long = 1;
            continue;
        }
        if (used + 2 > *capacity) {
            size_t next = *capacity ? *capacity : 256;
            if (next <= used + 1)
                next = next <= maximum_storage / 2 ? next * 2 : maximum_storage;
            if (next > maximum_storage) next = maximum_storage;
            char *grown = realloc(*buffer, next);
            if (!grown) return R2_INPUT_LINE_IO_ERROR;
            *buffer = grown;
            *capacity = next;
        }
        (*buffer)[used++] = (char)ch;
    }
    if (ch == EOF && ferror(stream)) return R2_INPUT_LINE_IO_ERROR;
    if (ch == EOF && used == 0) return R2_INPUT_LINE_EOF;
    if (ch == '\n' && used && (*buffer)[used - 1] == '\r') --used;
    if (too_long || used > max_bytes) {
        *out_length = max_bytes;
        return R2_INPUT_LINE_TOO_LONG;
    }
    if (!*capacity) {
        *buffer = malloc(1);
        if (!*buffer) return R2_INPUT_LINE_IO_ERROR;
        *capacity = 1;
    }
    (*buffer)[used] = '\0';
    *out_length = used;
    return embedded_nul ? R2_INPUT_LINE_EMBEDDED_NUL : R2_INPUT_LINE_OK;
}

#endif
