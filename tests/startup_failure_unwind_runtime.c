#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int r2_init(void);
void r2_shutdown(void);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s hands|diary|locked\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "locked")) {
        if (mkdir(R2_HOME, 0700) != 0 && errno != EEXIST) return 1;
        int lock = open(R2_HOME, O_RDONLY);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 1;
        pid_t child = fork();
        if (child == 0) {
            close(lock);
            int result = r2_init();
            r2_shutdown();
            _exit(result == -1 ? 0 : 1);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            access(R2_HOME "/r2_memory.db", F_OK) == 0) {
            fprintf(stderr, "a second core reached persistent initialization while the directory was locked\n");
            return 1;
        }
        close(lock);
        return 0;
    }

    const char *failure_env = NULL;
    if (strcmp(argv[1], "hands") == 0) {
        failure_env = "R2_TEST_FAIL_HANDS_THREAD_CREATE";
    } else if (strcmp(argv[1], "diary") == 0) {
        failure_env = "R2_TEST_FAIL_DIARY_THREAD_CREATE";
    } else {
        fprintf(stderr, "unknown startup failure scenario: %s\n", argv[1]);
        return 2;
    }

    if (setenv(failure_env, "1", 1) != 0) {
        perror("setenv");
        return 2;
    }

    if (r2_init() == 0) {
        fprintf(stderr, "r2_init unexpectedly succeeded with %s injected\n",
                failure_env);
        r2_shutdown();
        return 1;
    }

    /* This must be safe after either partial-startup failure. The test process
       exits only after the same centralized cleanup path used by main(). */
    r2_shutdown();
    int lock = open(R2_HOME, O_RDONLY);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "startup unwind left the persistent directory locked\n");
        return 1;
    }
    close(lock);
    return 0;
}
