/* A name can change after realpath validation. The actual production
 * workspace_read must never reopen a swapped symlink outside the workspace. */
#define _GNU_SOURCE
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdio.h>

static int swap_after_validation = 0;
static const char *const outside = "/tmp/r2-workspace-read-outside-fixture";

char *__real_realpath(const char *path, char *resolved);
char *__wrap_realpath(const char *path, char *resolved)
{
    char *result = __real_realpath(path, resolved);
    if (result && swap_after_validation &&
        strcmp(path, R2_WORKSPACE "/allowed.txt") == 0) {
        swap_after_validation = 0;
        if (unlink(path) != 0 || symlink(outside, path) != 0)
            abort();
    }
    return result;
}

int main(void)
{
    assert(mkdir(R2_WORKSPACE, 0700) == 0);
    assert(mkdir(R2_HOME, 0700) == 0);
    FILE *file = fopen(R2_WORKSPACE "/allowed.txt", "wb");
    assert(file && fputs("ordinary inside content", file) >= 0 && fclose(file) == 0);
    file = fopen(outside, "wb");
    assert(file && fputs("OUTSIDE_SECRET_SENTINEL", file) >= 0 && fclose(file) == 0);

    char *read = workspace_read("allowed.txt");
    assert(read && strcmp(read, "ordinary inside content") == 0);
    free(read);

    swap_after_validation = 1;
    read = workspace_read("allowed.txt");
    assert(!read);
    assert(swap_after_validation == 0);
    assert(unlink(R2_WORKSPACE "/allowed.txt") == 0);
    assert(unlink(outside) == 0);
    puts("Workspace read remains pinned after validated pathname swap.");
    return 0;
}
