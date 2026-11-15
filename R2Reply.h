#ifndef R2_REPLY_H
#define R2_REPLY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return a newly allocated user-facing reply with explicit private reasoning
   blocks removed. Returns NULL if no user-facing text remains. */
char *r2_reply_sanitize(const char *reply);
/* Preserve confirmed literal spans reached outside private or Markdown text.
   The callback returns the span length starting at the supplied line offset. */
char *r2_reply_sanitize_with_literals(const char *reply,
                                       size_t (*literal_span)(const char *, size_t));

#ifdef __cplusplus
}
#endif

#endif
