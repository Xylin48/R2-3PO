#ifndef R2_IMAGINATION_H
#define R2_IMAGINATION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize and shut down the hypothetical-experience subsystem. */
int r2_imagination_init(void);
void r2_imagination_shutdown(void);

/* Review complete context until the returned notes fit max_note_bytes.
 * Both outputs are caller-owned. processing_notes contains every review pass.
 * Context that already fits is copied without inference. Failure returns NULL. */
char *r2_review_context(const char *request, const char *context,
                         size_t max_note_bytes, char **processing_notes);

/*
 * Generate a context-informed hypothetical scenario and store it as a
 * clearly labeled Choice Lab branch. The returned string is heap allocated.
 */
char *r2_imagination_create(const char *request);

/*
 * Record explicit evidence-based feedback for a saved imagination branch.
 * Accepted assessments: accurate, partial, incorrect, unresolved.
 * Inaccurate/unresolved outcomes are not punished.
 * 0: saved and linked, 1: reward unavailable, 2: already rewarded.
 * 3: feedback event saved, branch link failed. Negative means rejected.
 */
int r2_imagination_feedback(long long branch_id, const char *assessment,
                            const char *notes);

#ifdef __cplusplus
}
#endif
#endif
