#ifndef R2_VISUAL_H
#define R2_VISUAL_H

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

/* Encode a real Eyes frame as a malloc-owned JPEG base64 string for direct
 * delivery to R2's multimodal conversation brain. This performs encoding only;
 * it does not interpret the image or create a visual memory. */
char *r2_visual_frame_base64(const R2VisionFrame *frame);

/* Analyze one real Eyes frame only when R2 deliberately requests inspection. */
char *r2_visual_analyze_frame(const R2VisionFrame *frame,
                              const char *source,
                              const char *question);

/* Read recent visual experiences as context; caller frees result. */
char *r2_visual_recent(int limit);
char *r2_visual_search(const char *query, int limit);

#ifdef __cplusplus
}
#endif
#endif
