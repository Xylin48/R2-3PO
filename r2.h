#ifndef R2_H
#define R2_H

#include <stddef.h>
#include <stdint.h>
#include "AudioDiscovery.h"

/*
 * ============================================================
 * R2-3PO CORE PUBLIC INTERFACE
 * ============================================================
 *
 * r2.c owns the actual implementation:
 *
 *   - Ollama / Qwen 3.5 multimodal conversation and vision
 *   - conversation
 *   - memory
 *   - relevant-memory retrieval
 *   - startup memory loading
 *   - autonomous thinking
 *   - tools
 *   - diary integration
 *   - Eyes (continuous raw-frame capture; the conversation brain receives images
 *     directly and chooses what to attend to; explicit inspection is separate)
 *   - Ears (continuous raw PCM capture; explicit speech recognition is separate,
 *     and capture itself never transcribes or classifies sound)
 *
 * This header is the PUBLIC bridge used by other R2 components,
 * especially shell.c.
 *
 * Do NOT put implementation here.
 * Do NOT duplicate R2 subsystems here.
 * ============================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Single source of truth for every local Ollama inference in R2. */
#define R2_OLLAMA_MODEL "qwen3.5:9b"

/* Shared RAM-aware context sizing for text and multimodal inference. */
int r2_ollama_context_tokens(void);
/* Resolve a validated loopback-only Ollama chat URL for text and vision. */
int r2_ollama_chat_url(char *out, size_t out_cap);


/* ============================================================
   CORE LIFECYCLE
   ============================================================ */

/*
 * Initialize the R2 core.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 *
 * The implementation uses the production initialization sequence shared
 * with the core executable; callers must not initialize subsystems separately.
 */
int r2_init(void);


/*
 * Shut down the R2 core cleanly.
 *
 * This allows the shell to request a normal R2 shutdown without
 * directly manipulating R2's internal threads/resources.
 */
void r2_shutdown(void);


/*
 * Returns non-zero while R2 is shutting down.
 */
int r2_is_shutting_down(void);

/*
 * Shared Ollama request gate. Vision uses this same gate as text generation
 * so the single local model is never asked to run competing inference jobs.
 * Vision requests are opportunistic and must yield when a foreground turn waits.
 */
int r2_ollama_vision_request_begin(void);
void r2_ollama_vision_request_end(void);
int r2_ollama_vision_request_should_abort(void);


/* ============================================================
   CONVERSATION
   ============================================================ */

/*
 * Send a message to R2 and receive its response.
 *
 * This is the public equivalent of entering a normal message
 * into R2's existing conversation system.
 *
 * The implementation in r2.c should continue to use:
 *
 *   message_add()
 *   chat_with_relevant_memories()
 *   process_tools()
 *   memory_decision()
 *   save_memory()
 *
 * so the shell does NOT create a second conversation system.
 *
 * Returned string is allocated by the R2 core and must be freed
 * by the caller with free().
 */
char *r2_talk(const char *message);
/* Same serialized conversation path, with an exact persisted speech-event link. */
char *r2_talk_with_speech_event(const char *message, int64_t speech_event_id);
/* Caller frees. Bounded display preview of recent human-facing turns. */
char *r2_recent_conversation_context(const char *exclude_latest, size_t max_chars);
/* Caller frees. Complete selected turns, with internal tool payloads excluded. */
char *r2_recent_conversation_context_complete(const char *exclude_latest);


/*
 * Run R2's normal interactive conversation loop.
 *
 * This exists so the shell can enter normal conversation mode
 * without duplicating the conversation implementation.
 */
int r2_conversation(void);


/* ============================================================
   THINKING
   ============================================================ */

/*
 * Ask R2 to perform one autonomous thinking cycle immediately.
 *
 * This uses R2's existing autonomous-thinking system.
 */
int r2_think(void);


/*
 * Start R2's autonomous thinking thread.
 *
 * Normally this is started during core initialization.
 */
int r2_start_thinking(void);


/*
 * Stop R2's autonomous thinking thread.
 */
void r2_stop_thinking(void);


/*
 * Returns non-zero if autonomous thinking is currently active.
 */
int r2_thinking_active(void);


/* ============================================================
   MEMORY
   ============================================================ */

/*
 * Retrieve memories relevant to a query.
 *
 * This uses R2's existing relevant-memory retrieval system.
 *
 * The shell can use this for commands such as:
 *
 *   memory search <query>
 *
 * Returned string must be freed by the caller.
 */
char *r2_retrieve_memories(const char *query);


/*
 * Save a memory through R2's existing memory system.
 *
 * category may be NULL if no category is required.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int r2_save_memory(const char *memory, const char *category);


/*
 * Load the startup memory set.
 *
 * R2 normally loads the newest startup memories during
 * initialization. This public function allows the shell to
 * request the operation without accessing the database directly.
 */
int r2_load_startup_memories(void);


/*
 * Return the number of memories currently known to the core,
 * when available.
 */
long r2_memory_count(void);


/* ============================================================
   TOOLS
   ============================================================ */

/*
 * Process an R2 tool command through the existing R2 tool system.
 *
 * The shell must not implement a second tool executor.
 */
char *r2_process_tools(const char *input);


/* ============================================================
   DIARY
   ============================================================ */

/*
 * Trigger a diary/autonomous-writing operation through the
 * existing diary system.
 *
 * r2.c remains responsible for connecting this to the existing
 * r2_diary subsystem.
 */
int r2_write_diary(void);

/* Sync adapter-verified events from the console command the caller selected. */
int r2_sync_gameboy_verified_events(const char *path);

/* Simulate a sleep transition and generate/store a private dream report. */
int r2_sleep_and_dream(double hours);
int r2_eat_and_learn(const char *food, double fullness);
int r2_eat_fridge_and_learn(const char *food, double fullness);


/*
 * Return whether the diary subsystem is currently available.
 */
int r2_diary_active(void);


/* ============================================================
   STATUS
   ============================================================ */

/*
 * Print a human-readable R2 status report.
 *
 * Intended for:
 *
 *   status
 *
 * from shell.c.
 */
void r2_status(void);


/*
 * Return whether the R2 core has been initialized.
 */
int r2_is_initialized(void);


/*
 * Return the model currently being used.
 *
 * Example:
 *
 *   Qwen 3.5 9B
 *
 * The returned pointer is owned by R2 and must NOT be freed.
 */
const char *r2_model_name(void);
/* Run a bounded, isolated local-model request through the shared Ollama gate.
 * Returns allocated response text or NULL; does not append to chat history. */
char *r2_model_generate(const char *system_prompt, const char *user_prompt, int num_predict);


/* ============================================================
   EYES
   ============================================================ */

/*
 * Tell the R2 core that the shell is requesting visual
 * functionality.
 *
 * The implementation must call the EXISTING Eyes subsystem.
 *
 * These functions are intentionally kept as bridges so shell.c
 * never needs to know how Eyes internally works.
 */
int r2_eyes_start(void);
/* Watch receives a token only for its own newly opened camera session. */
uint64_t r2_eyes_start_for_watch(void);
int r2_eyes_release_if_watch_owned(uint64_t token);
int r2_eyes_watch_owns(uint64_t token);
int r2_eyes_stop(void);
/* Watch-only release; disappearance is not a chosen sensory shutdown. */
int r2_eyes_release_source(void);
int r2_sense_auto_allowed(int which); /* 1=camera/vision, 2=mic, 3=desktop */
void r2_sense_set_auto_allowed(int which, int allowed);
int r2_eyes_status(void);

/* ============================================================
   VISUAL EXPERIENCE LIBRARY / VISION MODEL
   ============================================================ */

/* Analyze the current frame; opens the camera only for explicit vision requests. */
int r2_vision_available(void);
const char *r2_vision_model_name(void);
int r2_vision_watch_start(void);
int r2_vision_watch_stop(void);
int r2_vision_watch_active(void);
/* Discovery may claim only a worker it started; manual start revokes its claim. */
uint64_t r2_vision_watch_start_for_watch(void);
int r2_vision_watch_stop_if_watch_owned(uint64_t token);
int r2_vision_watch_owns(uint64_t token);
char *r2_vision_see(const char *question);
char *r2_vision_recent(int limit);
char *r2_vision_search(const char *query, int limit);
int r2_vision_set_model(const char *model);
int r2_vision_camera_available(void);
int r2_vision_vlc_available(void);
unsigned long r2_vision_vlc_window(void);
/* Generic X11 rendering window published by an active media source. */
int r2_vision_window_available(unsigned long window_id);
int r2_vision_window_available_cancelable(unsigned long window_id,
                                          int (*cancelled)(void *), void *userdata);
unsigned long r2_vision_window_for_pid(long pid);
int r2_vision_select_playback_window(const long *producers, size_t count,
                                    long *pid, unsigned long *window,
                                    int (*cancelled)(void *), void *userdata);
int r2_vision_open_window(unsigned long window_id);
/* Media is a separately observed display, not a replacement for Eyes. */
int r2_vision_media_open_window(unsigned long window_id);
int r2_vision_media_status(void);
/* R2's deliberate visual attention selection; media capture remains separate. */
int r2_vision_media_attention_status(void);
int r2_vision_media_set_attention(int watching);
int r2_vision_media_stop(void);
/* Watch alone may stop the exact media display generation it opened.
 * Explicit media replacement takes ownership away from Watch. */
uint64_t r2_vision_media_open_window_for_watch(unsigned long window_id);
uint64_t r2_vision_media_open_vlc_for_watch(void);
uint64_t r2_vision_media_open_vlc_window_for_watch(unsigned long window_id);
uint64_t r2_vision_media_open_x11_for_watch_cancelable(unsigned long window_id, int vlc,
                                                       int (*cancelled)(void *), void *userdata);
int r2_vision_media_stop_if_watch_owned(uint64_t token);
int r2_vision_media_watch_owns(uint64_t token);
char *r2_vision_media_see(const char *question);
int r2_vision_open_vlc(void);
int r2_vision_open_file(const char *path);
int r2_vision_close(void);


/* ============================================================
   EARS
   ============================================================ */

/*
 * Tell the R2 core that the shell is requesting audio/listening
 * functionality.
 *
 * The implementation must call the EXISTING Ears subsystem.
 */
int r2_ears_start(void);
/* Watch claims only a newly created stream. Zero means already owned or failed. */
uint64_t r2_ears_start_for_watch(const char *source);
uint64_t r2_ears_start_for_watch_cancelable(const char *source,
                                            int (*cancelled)(void *), void *userdata);
uint64_t r2_ears_start_desktop_for_watch(const char *monitor);
uint64_t r2_ears_start_desktop_for_watch_cancelable(const char *monitor,
                                                    int (*cancelled)(void *), void *userdata);
typedef struct {
    int active;
    uint64_t owner;
    char *source; /* caller frees the exact active source name */
} R2EarsWatchInput;
/* An unknown snapshot (-1) must not trigger a source replacement. */
int r2_ears_watch_input_snapshot(int desktop, R2EarsWatchInput *input);
/* Stop only the precise stream generation started by Watch; safe after replacement. */
int r2_ears_stop_if_watch_owned(uint64_t token);
int r2_ears_stop_desktop_if_watch_owned(uint64_t token);
/* Read the active monitor identity under the desktop stream lifecycle lock. */
/* Does the active microphone capture still use this exact source? */
int r2_ears_status_microphone_source(const char *expected_source);
int r2_ears_status_desktop_source(const char *monitor);
/* Caller frees the full selected source name, or receives NULL if inactive. */
char *r2_ears_current_desktop_source(void);
/* Capture the default sink monitor instead of the microphone. */
int r2_ears_start_desktop(void);
/* Independent desktop playback receiver; never replaces microphone capture. */
int r2_ears_stop_desktop(void);
int r2_ears_status_desktop(void);
/* Independent simultaneous desktop output captures. Watch owns these
 * supplemental sources; the ordinary desktop Ears API remains primary. */
int r2_ears_extra_reconcile(const R2AudioDiscovery *audio, const char *primary);
/* Cancellation predicates must be thread-safe, nonblocking reads of owner
 * state. Stop must also signal pending opens before joining the coordinator. */
int r2_ears_extra_reconcile_cancelable(const R2AudioDiscovery *audio, const char *primary,
                                      int (*cancelled)(void *), void *userdata);
void r2_ears_extra_stop_all(void);
/* Interrupt only pending supplementary opens before joining Watch. */
void r2_ears_extra_cancel_pending(void);
size_t r2_ears_extra_source_count(void);
int r2_ears_extra_copy_recent(const char *monitor, size_t bytes, void *output);
int r2_ears_stop(void);
int r2_ears_status(void);
/* Capture a short utterance and return a malloc-owned local ASR transcript.
   Requires R2_WHISPER_MODEL; seconds must be between 1 and 15. */
char *r2_ears_listen(unsigned seconds);
/* Cancelable variant for long-running sensory workers; callback returns nonzero to stop capture/ASR. */
char *r2_ears_listen_cancelable(unsigned seconds,
                                int (*cancelled)(void *userdata),
                                void *userdata);
/* Optional outputs are initialized to NULL/0; caller frees the full captured
 * source. Its identity is sampled with PCM, never after recognition finishes. */
char *r2_ears_listen_source_cancelable(unsigned seconds,
                                       int (*cancelled)(void *), void *userdata,
                                       char **source_out, int64_t *event_id_out);


/* ============================================================
   WATCH SUPPORT
   ============================================================ */

/*
 * Watch is the NEW subsystem.
 *
 * It will coordinate:
 *
 *   - microphone
 *   - system/desktop audio
 *   - VLC detection
 *   - VLC/video observation
 *
 * Watch itself belongs in shell/watch code.
 *
 * These functions allow the core to expose its state without
 * duplicating Eyes or Ears.
 */


/*
 * Returns non-zero if Watch mode is currently active.
 */
int r2_watch_active(void);


/*
 * Request Watch mode to begin.
 *
 * The actual Watch implementation will live outside the core.
 */
int r2_watch_start(void);


/*
 * Stop Watch mode.
 */
int r2_watch_stop(void);


/*
 * Print Watch status.
 */
void r2_watch_status(void);


/* ============================================================
   SHELL / COMMAND SUPPORT
   ============================================================ */

/*
 * Request a normal R2 shutdown.
 *
 * This is intentionally separate from r2_shutdown() so shell
 * commands can request shutdown without directly manipulating
 * the core lifecycle.
 */
void r2_request_shutdown(void);


/*
 * Request a restart/reboot of the R2 program.
 *
 * This is a program-level restart, NOT an operating-system
 * reboot.
 */
int r2_request_restart(void);


/* ============================================================
   THREAD / STATE INFORMATION
   ============================================================ */

/*
 * Return the current number of active R2 worker threads when
 * available.
 */
int r2_worker_count(void);


/*
 * Return the configured autonomous-thinking interval in seconds.
 */
int r2_think_interval(void);


/* ============================================================
   DEBUG / DIAGNOSTICS
   ============================================================ */

/*
 * Print diagnostic information about R2's internal systems.
 *
 * Intended for shell:
 *
 *   debug
 *   diagnostics
 *
 * This must report actual state rather than simulated values.
 */
void r2_diagnostics(void);


/* ============================================================
   END PUBLIC INTERFACE
   ============================================================ */

#ifdef __cplusplus
}
#endif

#endif /* R2_H */
