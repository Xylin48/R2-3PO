# Local speech input for R2 Ears

R2's Ears capture layer reads microphone PCM. The Qwen 3.5 chat endpoint used by
this project accepts text and images, not raw microphone PCM, so microphone
capture alone cannot make spoken words part of a conversation. The explicit
`ears listen` command connects the layers:

1. Capture a bounded 1–15 second microphone utterance as 48 kHz, stereo, 16-bit PCM.
2. Write a temporary WAV in a private `0700` directory.
3. Run a local `whisper.cpp` `whisper-cli` executable against a local GGML Whisper model.
4. Remove the temporary WAV and transcript.
5. Pass the recognized text to the same `r2_talk()` conversation path used by typed messages.

No audio is uploaded to a remote service, and temporary raw audio/transcript
files are removed after processing. The sensory event records that a
transcription occurred; the recognized text is then handled as a normal user
conversation turn, so it follows the existing chat history and memory path.

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
- `ears listen 8` — deliberately inspect an eight-second interval.
- `ears status` — inspect whether the raw Ears source is currently open.
- `ears stop` — stop continuous capture and clear its transient audio buffer.

The capture duration is limited to 1–15 seconds. Local transcription is
terminated if it exceeds 90 seconds or R2 begins shutting down. If the model
or executable is missing, R2 reports that speech recognition is unavailable
instead of pretending it understood audio.

## Watch coordinator

- `watch` or `watch start` opens continuous raw microphone capture and starts
  a coordinator that keeps sensory input active. It does not require Whisper.
- Watch refuses to start while Ears already has an active source, because that
  source could be a file or desktop monitor rather than the microphone. Stop
  the existing Ears source first; Watch does not silently replace it.
- While Watch is active, standalone `ears start`, `ears listen`, and
  `ears stop` commands are refused so they cannot compete with Watch's source.
  `ears status` remains available.
- Watch does not automatically transcribe speech or create chat turns from
  whatever happens to be audible. R2 must deliberately request a listening
  action to inspect an audio interval.
- `watch status` reports the coordinator thread, raw audio input, and visual
  input states. `watch stop` stops continuous microphone capture and joins the
  coordinator worker.
- If Eyes is already open, Watch may join its existing raw-frame capture.
  Watch never opens a camera or media source by itself and does not start a
  background image-description loop.

This provides continuous raw PCM capture plus deliberate speech recognition.
It is not yet semantic perception of non-speech sounds: Qwen 3.5 9B does not
accept raw audio, and the current explicit Whisper bridge recognizes speech
rather than classifying every environmental sound.

This wiring is covered by a static CI regression check. A passing CI build
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
