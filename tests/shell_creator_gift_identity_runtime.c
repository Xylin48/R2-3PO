/* Execute the real shell creator-gift path with fixed wall time. A second
 * command in the same second must not overwrite or merge earlier money items.
 * Other shell subsystems are discarded by --gc-sections for this fixture. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../shell.c"

static char gift_names[12][96];
static int gifts = 0;

time_t time(time_t *when)
{
    const time_t fixed = (time_t)1700000000;
    if (when) *when = fixed;
    return fixed;
}

int r2_reality_add_item(const char *name, const char *description,
                        const char *container, int quantity)
{
    assert(description && *description);
    assert(!strcmp(container, "pockets"));
    assert(quantity == 1);
    assert(gifts < (int)(sizeof(gift_names)/sizeof(gift_names[0])));
    size_t n = strlen(name);
    assert(n > 0 && n < sizeof(gift_names[0]));
    for (int i = 0; i < gifts; ++i) assert(strcmp(gift_names[i], name) != 0);
    memcpy(gift_names[gifts++], name, n + 1);
    return 0;
}

int r2_log_is_initialized(void)
{
    return 0;
}

int64_t r2_log_event_with_memory(R2LogCategory category, const char *event_type,
                                 const char *summary, const char *details,
                                 const char *source, int save_as_memory)
{
    (void)category; (void)event_type; (void)summary; (void)details;
    (void)source; (void)save_as_memory;
    assert(!"A disabled Life Log must not receive a creator gift event");
    return -1;
}

int main(void)
{
    assert(shell_give("money 3") == 1);
    assert(shell_give("money 3") == 1);
    assert(shell_give("money 2") == 1);
    assert(gifts == 8);
    puts("Same-second creator money gifts remain separate physical item identities.");
    return 0;
}
