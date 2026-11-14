/* Verify the production dream prompt builder preserves every input byte.
 * No model is required: the full context is assembled before inference. */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char *large_context(char fill, const char *tail, size_t prefix)
{
    size_t end = strlen(tail);
    char *value = malloc(prefix + end + 1);
    assert(value);
    memset(value, (unsigned char)fill, prefix);
    memcpy(value + prefix, tail, end + 1);
    return value;
}

int main(void)
{
    const char *tails[] = {
        "END_OF_WHOLE_REALITY_123",
        "END_OF_WHOLE_MEMORY_456",
        "END_OF_PRIVATE_DIARY_789",
        "END_OF_REWARD_EVIDENCE_012",
        "END_OF_REPEAT_INTEREST_345"
    };
    size_t lengths[] = {9000, 12000, 15000, 7000, 6000};
    char *inputs[5] = {0};
    size_t total = 0;
    for (size_t i = 0; i < 5; ++i) {
        inputs[i] = large_context((char)('A' + (int)i), tails[i], lengths[i]);
        total += strlen(inputs[i]);
    }
    char *dream = r2_build_dream_context(
        inputs[0], inputs[1], inputs[2], inputs[3], inputs[4]);
    assert(dream && strlen(dream) > total);
    for (size_t i = 0; i < 5; ++i) {
        assert(strstr(dream, inputs[i]) != NULL);
        assert(strstr(dream, tails[i]) != NULL);
        free(inputs[i]);
    }
    assert(strstr(dream, "PRIVATE DREAM SIMULATION CONTEXT"));
    assert(strstr(dream, "not a command or proof of a waking event"));
    free(dream);
    puts("Dream prompt preserves complete state, memory, diary, reward and interest context.");
    return 0;
}
