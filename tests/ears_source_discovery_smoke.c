#define _POSIX_C_SOURCE 200809L
#include "Ears.h"
#include "Log.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Discovery must work without opening any real microphone or speaker. */
int64_t r2_log_sensory(const char *sense, const char *observation,
                      const char *details, const char *source)
{
    (void)sense; (void)observation; (void)details; (void)source;
    return 0;
}

int main(void)
{
    char root[] = "/tmp/r2-source-discovery-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char script[PATH_MAX], augmented_path[PATH_MAX * 2];
    assert(snprintf(script, sizeof(script), "%s/pactl", root) < (int)sizeof(script));
    FILE *fp = fopen(script, "w");
    assert(fp);
    const char *body =
        "#!/bin/sh\n"
        "if [ \"$1\" = \"get-default-source\" ]; then\n"
        "  echo alsa_input.test_mic\n"
        "elif [ \"$1\" = \"list\" ] && [ \"$2\" = \"short\" ]; then\n"
        "  echo '0 alsa_output.test.monitor module-null-sink.c'\n"
        "  if [ \"$R2_TEST_MIC\" = \"1\" ]; then\n"
        "    echo '1 alsa_input.test_mic module-alsa-card.c'\n"
        "  fi\n"
        "elif [ \"$1\" = \"list\" ] && [ \"$2\" = \"sink-inputs\" ]; then\n"
        "  echo 'Sink Input #21'\n"
        "  if [ \"$R2_TEST_PLAYING\" = \"1\" ]; then echo '    Corked: no'\n"
        "  else echo '    Corked: yes'; fi\n"
        "  echo '    application.process.id = \"3141\"'\n"
        "  echo 'Sink Input #22'\n"
        "  echo '    Corked: yes'\n"
        "  echo '    application.process.id = \"2718\"'\n"
        "fi\n";
    assert(fputs(body, fp) >= 0);
    assert(fclose(fp) == 0);
    assert(chmod(script, 0700) == 0);
    const char *old_path = getenv("PATH");
    assert(snprintf(augmented_path, sizeof(augmented_path), "%s:%s", root,
                    old_path ? old_path : "/usr/bin") < (int)sizeof(augmented_path));
    assert(setenv("PATH", augmented_path, 1) == 0);

    assert(setenv("R2_TEST_MIC", "0", 1) == 0);
    assert(setenv("R2_TEST_PLAYING", "0", 1) == 0);
    assert(r2_ears_microphone_available() == 0);
    assert(r2_ears_desktop_playing() == 0);
    assert(r2_ears_playback_pid() == -1);

    assert(setenv("R2_TEST_MIC", "1", 1) == 0);
    assert(r2_ears_microphone_available() == 1);
    assert(r2_ears_desktop_playing() == 0);
    assert(r2_ears_playback_pid() == -1);

    assert(setenv("R2_TEST_PLAYING", "1", 1) == 0);
    assert(r2_ears_microphone_available() == 1);
    assert(r2_ears_desktop_playing() == 1);
    assert(r2_ears_playback_pid() == 3141);

    assert(setenv("R2_TEST_MIC", "0", 1) == 0);
    assert(r2_ears_microphone_available() == 0);
    assert(r2_ears_desktop_playing() == 1);
    assert(r2_ears_playback_pid() == 3141);

    assert(unlink(script) == 0);
    assert(rmdir(root) == 0);
    puts("Watch audio source probes distinguish hotplugged microphones and uncorked playback.");
    return 0;
}
