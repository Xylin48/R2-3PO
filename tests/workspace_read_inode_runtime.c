/* A name can change after realpath validation. The actual production
 * workspace_read must never reopen a swapped symlink outside the workspace. */
#define _GNU_SOURCE
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

static int swap_after_validation = 0;
static int swap_directory_after_validation = 0;
static const char *const outside = "/tmp/r2-workspace-read-outside-fixture";
static const char *const outside_dir = "/tmp/r2-workspace-list-outside-fixture";

/* Hook the final descriptor open, after safe_path() has returned, so a
 * linker/stdlib realpath alias cannot make the race fixture a false pass. */
int __real_openat(int dirfd, const char *path, int flags, ...);
int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    if (swap_after_validation && !strcmp(path, "allowed.txt") &&
        (flags & O_NOFOLLOW)) {
        swap_after_validation = 0;
        if (unlink(R2_WORKSPACE "/allowed.txt") != 0 ||
            symlink(outside, R2_WORKSPACE "/allowed.txt") != 0)
            abort();
    }
    if (swap_directory_after_validation && !strcmp(path, "folder") &&
        (flags & O_DIRECTORY) && (flags & O_NOFOLLOW)) {
        swap_directory_after_validation = 0;
        if (rename(R2_WORKSPACE "/folder", R2_WORKSPACE "/old-folder") != 0 ||
            symlink(outside_dir, R2_WORKSPACE "/folder") != 0)
            abort();
    }
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        va_end(args);
        return __real_openat(dirfd, path, flags, mode);
    }
    return __real_openat(dirfd, path, flags);
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

    assert(mkdir(R2_WORKSPACE "/folder", 0700) == 0);
    assert(mkdir(outside_dir, 0700) == 0);
    file = fopen(R2_WORKSPACE "/folder/inside.txt", "wb");
    assert(file && fputs("safe", file) >= 0 && fclose(file) == 0);
    file = fopen("/tmp/r2-workspace-list-outside-fixture/PRIVATE_SENTINEL", "wb");
    assert(file && fputs("outside", file) >= 0 && fclose(file) == 0);
    char *listed = workspace_list("folder");
    assert(listed && strstr(listed, "inside.txt") && !strstr(listed, "PRIVATE_SENTINEL"));
    free(listed);

    swap_directory_after_validation = 1;
    listed = workspace_list("folder");
    assert(!listed && swap_directory_after_validation == 0);
    free(listed);
    assert(unlink(R2_WORKSPACE "/folder") == 0);
    assert(unlink(R2_WORKSPACE "/old-folder/inside.txt") == 0);
    assert(rmdir(R2_WORKSPACE "/old-folder") == 0);
    assert(unlink("/tmp/r2-workspace-list-outside-fixture/PRIVATE_SENTINEL") == 0);
    assert(rmdir(outside_dir) == 0);
    puts("Workspace read and list both refuse swapped outside-workspace symlinks.");
    return 0;
}
