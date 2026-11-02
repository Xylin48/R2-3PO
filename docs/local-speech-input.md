# Local speech input for R2 Ears

R2's Ears capture layer reads microphone PCM. The Qwen 3.5 chat endpoint used by
this project accepts text and images, not raw microphone PCM, so microphone
capture alone cannot make spoken words part of a conversation. The explicit
`ears listen` command connects the layers:

1. Keep the microphone and desktop playback as independent continuous 48 kHz, stereo, 16-bit PCM streams; deliberate listening selects one exact 1–15 second interval without changing either live source.
2. Write that captured interval to a temporary raw WAV in a private `0700` directory.
3. Convert **only the completed temporary clip** to 16 kHz, mono, 16-bit PCM WAV for `whisper.cpp` (using an exact 3:1 downsampling ratio and averaging stereo sample groups). The original audio buffers and live capture formats remain unchanged.
4. Run a local `whisper.cpp` `whisper-cli` executable against the converted WAV and a local GGML Whisper model.
5. Remove both temporary WAV files and the transcript.
6. Pass the recognized text to the same `r2_talk()` conversation path used by typed messages.

No audio is uploaded to a remote service, and temporary raw audio/transcript
files are removed after processing. A `speech_transcribed` sensory event keeps
the complete recognized text and the exact source identity captured with the PCM,
even if the device changes while recognition runs. Shell speech follows the normal
conversation path and links its turn to that exact event ID. A model-selected
listening action returns evidence to its current turn or reflection; it does not
invent a new user message. Life Log and Sensory Journal keep complete selected
records rather than cutting transcript or conversation text to a fixed buffer.

## Configure it

Install/build `whisper.cpp` locally and obtain a compatible GGML Whisper model
through your normal trusted process. This feature deliberately does not
download models or use a remote speech API.

Make the model readable by the dedicated Linux user `r2`, and ensure the
Whisper CLI is executable by that user. Then export these variables before
launching R2:

```sh
export R2_WHISPER_MODEL=/home/x/R2_Home/models/ggml-base.en.bin
export R2_WHISPER_CLI=/usr/local/bin/whisper-cli
/home/x/R2_Home/R2_Launch_Code.sh
```

Adjust the paths to match the actual installed model and executable. If
`R2_WHISPER_CLI` is unset, R2 looks for `whisper-cli` on its PATH.
The launcher explicitly passes both variables through to R2's dedicated user.

## Use it

At the R2 prompt:

- `ears start` — open the microphone and continuously receive raw PCM into a
  16-second in-memory rolling buffer. The capture worker does not transcribe,
  classify, or interpret the audio.
- `ears listen` — deliberately capture the next five seconds from the live
  input (or directly record five seconds if continuous Ears is not already
  running), transcribe that interval locally, and send the resulting text to chat.
- `ears listen 8` — deliberately record and transcribe the next eight-second interval.
- `ears status` — inspect whether the raw Ears source is currently open.

When the brain itself decides that a real sound deserves attention, it can request
`[EARS_LISTEN] recent|5` to inspect the most recent five seconds already in the
rolling buffer, or `[EARS_LISTEN] live|5` to listen to the next five seconds.
This is a deliberate tool action, not an automatic consequence of microphone
capture. Recent-buffer listening requires Ears continuous capture to be active.
- `ears stop` — stop continuous capture and clear its transient audio buffer.

The capture duration is limited to 1–15 seconds. Local transcription is
terminated if it exceeds 90 seconds or R2 begins shutting down. If the model
or executable is missing, R2 reports that speech recognition is unavailable
instead of pretending it understood audio.

## Watch coordinator

- `watch` or `watch start` begins source discovery even when no input is currently
  available. It does not require Whisper. Available permitted microphones,
  physical cameras and playback monitors can then start their independent raw
  feeds; the camera remains primary Eyes and media uses a separate display.
- Multiple active playback sinks keep separate monitor identities and PCM
  histories. `[EARS_SOURCES]` lists the active primary and additional monitors.
  `[EARS_LISTEN] desktop_source_recent|exact-monitor|5` or
  `desktop_source_live|exact-monitor|5` selects a named monitor, including the
  current primary. The requested interval cannot cross a source generation.
- Watch can coexist with manually opened sources. Explicit `ears start`,
  `ears listen`, and `ears stop` remain available; a deliberate sensory close
  prevents Watch from reopening that input until it is permitted again.
- Raw availability never automatically transcribes speech, classifies a room
  sound or forces an attention change. R2 chooses whether to inspect it.
- `watch status` reports discovery and independent input states. `watch stop`
  cancels pending Watch-owned opens and joins the coordinator, then releases
  only captures still owned by that exact Watch generation. Manual replacements
  are preserved. Shutdown also wakes a blocked direct microphone capture.
- A failed discovery query preserves existing inputs rather than claiming they
  disappeared. Confirmed route changes can replace only Watch-owned captures.


This provides continuous raw PCM capture plus deliberate speech recognition.
It is not yet semantic perception of non-speech sounds: Qwen 3.5 9B does not
accept raw audio, and the current explicit Whisper bridge recognizes speech
rather than classifying every environmental sound.

The WAV conversion is exercised by a compiled C regression that checks the resulting file header, exact sample count, stereo-to-mono signal values, original source integrity, and failure on incomplete input; static wiring tests check the normal chat handoff. A passing CI build
does not prove that a particular microphone, PulseAudio/PipeWire setup, Whisper
binary, or model works on the target machine; those still need a local test.


## Camera device selection

Eyes defaults to `/dev/video0`. If the intended camera or capture device has a
different node, set `R2_CAMERA_DEVICE` before launching R2, for example:

```sh
export R2_CAMERA_DEVICE=/dev/video2
/home/x/R2_Home/R2_Launch_Code.sh
```

The selected V4L2 device must support the current capture mode (640×480 at
approximately 10 frames per second) and be readable/writable by the `r2`
runtime user. Use the hardware preflight to enumerate available devices, then
test `vision see` and a visual question in chat on the actual machine.
