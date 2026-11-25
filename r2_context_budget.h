#ifndef R2_CONTEXT_BUDGET_H
#define R2_CONTEXT_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* Shared text-model allocation policy. The byte/token ratio is an estimate,
 * not an exact tokenizer. Heap allocation and actual JSON bytes are separate. */
#define R2_TEXT_TEMPLATE_RESERVE 512
#define R2_TEXT_MIN_GENERATION 64
#define R2_TEXT_BYTE_NUMERATOR 32U
#define R2_TEXT_BYTE_DENOMINATOR 10U

static inline int r2_text_predict_limit(int ctx, int requested)
{
    int ceiling = ctx >= 16384 ? 8192 :
                  ctx >= 12288 ? 6144 :
                  ctx >= 8192 ? 4096 :
                  ctx >= 4096 ? 2048 : 512;
    int headroom = ctx > 1536 ? ctx - 1536 : 256;
    if (ceiling > headroom) ceiling = headroom;
    if (requested < 1) requested = ctx >= 12288 ? 2048 : 1024;
    return requested > ceiling ? ceiling : requested;
}

/* Accept an already allocated generation budget, including a budget reduced
 * to fit complete current input. Do not clamp it a second time here. */
static inline size_t r2_text_input_bytes(int ctx, int generation)
{
    if (ctx <= R2_TEXT_TEMPLATE_RESERVE || generation < 0 ||
        generation > ctx - R2_TEXT_TEMPLATE_RESERVE)
        return 0;
    uint64_t bytes = (uint64_t)(ctx - R2_TEXT_TEMPLATE_RESERVE - generation) *
                     R2_TEXT_BYTE_NUMERATOR / R2_TEXT_BYTE_DENOMINATOR;
    return bytes > SIZE_MAX ? SIZE_MAX : (size_t)bytes;
}

#endif
