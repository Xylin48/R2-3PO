#ifndef R2_VISUAL_H
#define R2_VISUAL_H

#include <stddef.h>
#include <stdint.h>
#include "Eyes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * R2 Visual Experience Library + Ollama vision-model bridge.
 * Images and descriptions are stored separately from diary and
 * conversational-memory tables.
 */
int r2_visual_init(const char *database_path, const char *library_directory);
void r2_visual_shutdown(void);
int r2_visual_is_initialized(void);
int r2_visual_set_model(const char *model);
const char *r2_visual_model_name(void);

/* Encode a captured Eyes or display frame as malloc-owned JPEG base64.
 * Encoding alone performs no recognition and creates no visual memory. */
char *r2_visual_frame_base64(const R2VisionFrame *frame);

/* Interpret an already captured JPEG through the visual model for the text-only
 * conversation brain. Returns an owned description, or NULL when unavailable.
 * The caller preserves the source identity and supplies it as evidence. */
char *r2_visual_describe_encoded_frame(const char *jpeg_base64, const char *question);

/* Analyze one real Eyes frame only when R2 deliberately requests inspection. */
char *r2_visual_analyze_frame(const R2VisionFrame *frame,
                              const char *source,
                              const char *question);

/* Read recent visual experiences as context; caller frees result. */
char *r2_visual_recent(int limit);
char *r2_visual_search(const char *query, int limit);

/* Visit complete rows by SQLite ID. Row text is borrowed for the callback.
 * Return nonzero from the callback to stop. Discard partial results on error.
 * Callbacks must not reenter the provider. */
int r2_visual_visit_search(const char *query, int limit,
    int (*visit)(void *context, int64_t id, const char *row, size_t length),
    void *context);

int r2_visual_visit_recent(int limit,
    int (*visit)(void *context, int64_t id, const char *row, size_t length),
    void *context);

#ifdef __cplusplus
}
#endif
#endif
