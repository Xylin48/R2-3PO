#define _GNU_SOURCE
#include "R2Reply.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *open;
    const char *close;
} HiddenBlock;

static const HiddenBlock hidden_blocks[] = {
    {"<think>", "</think>"},
    {"<analysis>", "</analysis>"},
    {"<reasoning>", "</reasoning>"},
    {"<scratchpad>", "</scratchpad>"},
    {"<internal>", "</internal>"},
    {"<private_thought>", "</private_thought>"},
    {"<|thinking|>", "<|/thinking|>"},
    {"<|analysis|>", "<|/analysis|>"}
};

static const char *find_hidden_open(const char *p, const HiddenBlock **found)
{
    for (size_t i = 0; i < sizeof(hidden_blocks) / sizeof(hidden_blocks[0]); ++i) {
        size_t n = strlen(hidden_blocks[i].open);
        if (!strncasecmp(p, hidden_blocks[i].open, n)) {
            *found = &hidden_blocks[i];
            return p + n;
        }
    }
    return NULL;
}

static int is_hidden_close(const char *p, size_t *length)
{
    for (size_t i = 0; i < sizeof(hidden_blocks) / sizeof(hidden_blocks[0]); ++i) {
        size_t n = strlen(hidden_blocks[i].close);
        if (!strncasecmp(p, hidden_blocks[i].close, n)) {
            if (length) *length = n;
            return 1;
        }
    }
    return 0;
}

/* Track nested private-tag types, not just repetitions of the outer tag.
   A malformed crossing such as <think><analysis></think>...</analysis></think>
   must not close the outer block early and expose the intervening text. */
static const char *find_hidden_close_balanced_until(const char *p,
                                                     const HiddenBlock *block,
                                                     const char *limit,
                                                     const HiddenBlock **stack,
                                                     size_t capacity)
{
    if (!capacity) return NULL;
    size_t depth = 1;
    stack[0] = block;
    while (*p && (!limit || p < limit)) {
        if (*p != '<') {
            ++p;
            continue;
        }
        const HiddenBlock *opened = NULL;
        const char *after_open = find_hidden_open(p, &opened);
        if (after_open && (!limit || after_open <= limit)) {
            if (depth >= capacity) return NULL;
            stack[depth++] = opened;
            p = after_open;
            continue;
        }

        size_t close_len = 0;
        if (is_hidden_close(p, &close_len)) {
            if (limit && (size_t)(limit - p) < close_len) {
                ++p;
                continue;
            }
            const char *expected = stack[depth - 1]->close;
            if (!strncasecmp(p, expected, strlen(expected))) {
                --depth;
                if (depth == 0) return p;
            }
            /* A close for an outer tag cannot close through an unclosed
               nested tag. Ignore it until the nested block is closed. */
            p += close_len;
            continue;
        }
        ++p;
    }
    return NULL;
}

static char *trim_reply(char *text)
{
    char *start = text;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);

    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
    return text;
}

/* Markdown code is user-facing syntax, not permission to expose private
   contents. Preserve tag delimiters in examples while redacting their body. */
static const char *reply_find_matching_ticks(const char *text, size_t length,
                                                size_t after, size_t run_length)
{
    size_t pos = after;
    while (pos < length) {
        if (text[pos] != '`') {
            ++pos;
            continue;
        }
        size_t run = 1;
        while (pos + run < length && text[pos + run] == '`') ++run;
        if (run == run_length) return text + pos;
        pos += run;
    }
    return NULL;
}

static int reply_fence_line(const char *line, size_t length,
                            char expected_char, size_t expected_length,
                            int closing, char *found_char, size_t *found_length)
{
    size_t pos = 0;
    while (pos < length && line[pos] == ' ' && pos < 4) ++pos;
    if (pos > 3 || pos >= length ||
        (line[pos] != '`' && line[pos] != '~'))
        return 0;

    char ch = line[pos];
    size_t run = 0;
    while (pos + run < length && line[pos + run] == ch) ++run;
    if (run < 3) return 0;

    if (closing) {
        if (ch != expected_char || run < expected_length) return 0;
        pos += run;
        while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        if (pos != length) return 0;
    } else if (ch == '`') {
        for (size_t i = pos + run; i < length; ++i)
            if (line[i] == '`') return 0;
    }

    if (found_char) *found_char = ch;
    if (found_length) *found_length = run;
    return 1;
}

static const char *reply_find_fence_close(const char *text, size_t length,
                                          size_t from, char fence_char,
                                          size_t fence_length)
{
    size_t pos = from;
    while (pos < length) {
        if (pos != 0 && text[pos - 1] != '\n') {
            ++pos;
            continue;
        }
        size_t line_end = pos;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t check_length = line_end - pos;
        if (check_length && text[pos + check_length - 1] == '\r')
            --check_length;
        if (reply_fence_line(text + pos, check_length,
                             fence_char, fence_length, 1, NULL, NULL))
            return text + pos;
        pos = line_end < length ? line_end + 1 : line_end;
    }
    return text + length;
}

/* Return the answer after a line label outside Markdown code. */
static const char *reply_find_final_content(const char *text,
                                             size_t (*literal_span)(const char *, size_t))
{
    size_t length = strlen(text);
    size_t pos = 0;
    while (pos < length) {
        if (pos == 0 || text[pos - 1] == '\n') {
            size_t span = literal_span ? literal_span(text, pos) : 0;
            if (span && span <= length - pos) {
                pos += span;
                continue;
            }
            size_t line_end = pos;
            while (line_end < length && text[line_end] != '\n') ++line_end;
            size_t line_length = line_end - pos;
            if (line_length && text[line_end - 1] == '\r') --line_length;
            char fence;
            size_t fence_length;
            if (reply_fence_line(text + pos, line_length, 0, 0, 0,
                                 &fence, &fence_length)) {
                const char *close = reply_find_fence_close(
                    text, length, line_end < length ? line_end + 1 : length,
                    fence, fence_length);
                pos = (size_t)(close - text);
                while (pos < length && text[pos] != '\n') ++pos;
                if (pos < length) ++pos;
                continue;
            }
            if (pos != 0 && !strncasecmp(text + pos, "final:", 6))
                return text + pos + 6;
        }
        if (text[pos] == '`') {
            size_t run = 1;
            while (pos + run < length && text[pos + run] == '`') ++run;
            const char *close = reply_find_matching_ticks(text, length, pos + run, run);
            pos = close ? (size_t)(close - text) + run : pos + run;
            continue;
        }
        ++pos;
    }
    return NULL;
}

char *r2_reply_sanitize(const char *reply)
{
    return r2_reply_sanitize_with_literals(reply, NULL);
}

char *r2_reply_sanitize_with_literals(const char *reply,
                                       size_t (*literal_span)(const char *, size_t))
{
    if (!reply) return NULL;
    size_t input_len = strlen(reply);
    if (!input_len) return NULL;

    /* Classify labeled scratch only after private tags have been removed.
       A Final: line inside a hidden block is never an answer boundary. */
    const char *start = reply;
    static const char *const scratch_prefixes[] = {
        "we need answer", "we need to answer", "we need respond",
        "we need to respond", "we should answer", "we should respond",
        "we need provide", "we need to provide", "we should provide",
        "let's craft", "let us craft", "let me formulate",
        "i need to answer", "i should answer", "i will respond",
        "i'll answer", "the user asks", "the user wants",
        "we need make sure", "we need ensure", NULL
    };
    size_t len = strlen(start);
    size_t block_count = 0;
    for (size_t i = 0; i < len; ++i) {
        if (start[i] == '<') {
            const HiddenBlock *ignored = NULL;
            if (find_hidden_open(start + i, &ignored)) ++block_count;
        }
    }
    /* Code examples keep their tag delimiters but replace the body with a
       fixed placeholder, so reserve space for every possible expansion. */
    if (block_count > ((size_t)-1 - len - 1) / 32 ||
        block_count > (size_t)-1 / sizeof(const HiddenBlock *)) return NULL;
    char *clean = malloc(len + block_count * 32 + 1);
    if (!clean) return NULL;
    /* The initial tag count also bounds nesting depth. Reuse one stack so
       each block only scans through its own matching close. */
    const HiddenBlock **stack = block_count ? malloc(block_count * sizeof(*stack)) : NULL;
    if (block_count && !stack) {
        free(clean);
        return NULL;
    }

    size_t src = 0, dst = 0;
    size_t inline_ticks = 0;
    size_t inline_close_pos = (size_t)-1;
    int in_fence = 0;
    char fence_char = 0;
    size_t fence_length = 0;
    size_t fence_close_after = (size_t)-1;
    const char *fence_close = NULL;
    while (src < len) {
        if (fence_close_after != (size_t)-1 && src >= fence_close_after) {
            in_fence = 0;
            fence_close_after = (size_t)-1;
        }
        /* Recognize Markdown fences only at line boundaries and outside
           inline code; private tag bodies are still redacted within examples. */
        if (inline_ticks == 0 && (src == 0 || start[src - 1] == '\n')) {
            size_t span = !in_fence && literal_span ? literal_span(start, src) : 0;
            if (span && span <= len - src) {
                memcpy(clean + dst, start + src, span);
                dst += span;
                src += span;
                continue;
            }
            size_t line_end = src;
            while (line_end < len && start[line_end] != '\n') ++line_end;
            size_t line_length = line_end - src;
            size_t check_length = line_length;
            if (check_length && start[src + check_length - 1] == '\r')
                --check_length;
            size_t after_line = line_end < len ? line_end + 1 : line_end;
            if (in_fence) {
                char ignored_char = 0;
                size_t ignored_length = 0;
                if (reply_fence_line(start + src, check_length,
                                     fence_char, fence_length, 1,
                                     &ignored_char, &ignored_length))
                    fence_close_after = after_line;
            }
            char opening_char = 0;
            size_t opening_length = 0;
            if (!in_fence &&
                reply_fence_line(start + src, check_length, 0, 0, 0,
                                 &opening_char, &opening_length)) {
                size_t indent = 0;
                while (indent < check_length && start[src + indent] == ' ')
                    ++indent;
                size_t prefix_length = indent + opening_length;
                memcpy(clean + dst, start + src, prefix_length);
                dst += prefix_length;
                in_fence = 1;
                fence_char = opening_char;
                fence_length = opening_length;
                fence_close = NULL;
                src += prefix_length;
                continue;
            }
        }

        if (!in_fence && start[src] == '`') {
            size_t run = 1;
            while (src + run < len && start[src + run] == '`') ++run;
            if (inline_ticks == 0) {
                const char *matching = reply_find_matching_ticks(start, len, src + run, run);
                if (matching) {
                    inline_ticks = run;
                    inline_close_pos = (size_t)(matching - start);
                }
            } else if (inline_ticks == run) {
                inline_ticks = 0;
                inline_close_pos = (size_t)-1;
            }
            memcpy(clean + dst, start + src, run);
            src += run;
            dst += run;
            continue;
        }

        int inside_code = in_fence || inline_ticks != 0;
        const HiddenBlock *block = NULL;
        const char *after_open = start[src] == '<' ? find_hidden_open(start + src, &block) : NULL;
        if (after_open) {
            const char *code_limit = NULL;
            if (in_fence) {
                if (!fence_close)
                    fence_close = reply_find_fence_close(start, len, src, fence_char, fence_length);
                code_limit = fence_close;
            } else if (inline_ticks != 0 && inline_close_pos != (size_t)-1)
                code_limit = start + inline_close_pos;
            if (inside_code && code_limit && after_open > code_limit) {
                clean[dst++] = start[src++];
                continue;
            }
            const char *close = find_hidden_close_balanced_until(
                after_open, block, inside_code ? code_limit : NULL, stack, block_count);
            if (!close) {
                if (inside_code) {
                    size_t open_len = (size_t)(after_open - (start + src));
                    memcpy(clean + dst, start + src, open_len);
                    dst += open_len;
                    static const char omitted[] = "[unclosed content omitted]";
                    memcpy(clean + dst, omitted, sizeof(omitted) - 1);
                    dst += sizeof(omitted) - 1;
                    /* Never pair a literal code opener with a close token
                       outside this code span. Skip only the ambiguous code
                       remainder, then resume at its closing delimiter. */
                    if (in_fence)
                        src = (size_t)(code_limit - start);
                    else if (inline_close_pos != (size_t)-1)
                        src = inline_close_pos;
                    else
                        src = len;
                    continue;
                }
                /* An unclosed private block is never displayed verbatim. */
                break;
            }
            if (inside_code) {
                size_t open_len = (size_t)(after_open - (start + src));
                memcpy(clean + dst, start + src, open_len);
                dst += open_len;
                static const char omitted[] = "[content omitted]";
                memcpy(clean + dst, omitted, sizeof(omitted) - 1);
                dst += sizeof(omitted) - 1;
                size_t close_len = strlen(block->close);
                memcpy(clean + dst, close, close_len);
                dst += close_len;
            }
            src = (size_t)(close - start) + strlen(block->close);
            continue;
        }

        size_t close_len = 0;
        if (start[src] == '<' && !inside_code && is_hidden_close(start + src, &close_len)) {
            src += close_len;
            continue;
        }

        clean[dst++] = start[src++];
    }
    free(stack);
    clean[dst] = '\0';
    trim_reply(clean);

    /* Models may emit labeled scratch without tags, including after a
       tagged private preamble. Only inspect the remaining visible text. */
    const char *visible = clean;
    while (*visible && isspace((unsigned char)*visible)) ++visible;
    int visible_looks_private =
        !strncasecmp(visible, "analysis:", 9) ||
        !strncasecmp(visible, "reasoning:", 10) ||
        !strncasecmp(visible, "thinking:", 9);
    for (size_t i = 0; !visible_looks_private && scratch_prefixes[i]; ++i) {
        size_t n = strlen(scratch_prefixes[i]);
        if (!strncasecmp(visible, scratch_prefixes[i], n))
            visible_looks_private = 1;
    }
    if (visible_looks_private) {
        const char *final_content = reply_find_final_content(visible, literal_span);
        if (!final_content) {
            free(clean);
            return NULL;
        }
        memmove(clean, final_content, strlen(final_content) + 1);
        trim_reply(clean);
    }

    if (!*clean) {
        free(clean);
        return NULL;
    }
    return clean;
}
