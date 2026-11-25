# R2 rollout-readiness gate

First launch on the real workspace is the final deployment step. Complete code review, isolated runtime tests, and hardware checks before installation. Keep the live databases, diary, and original conversation archive out of test runs.

## 1. Verify the candidate behavior

- Record the exact source commit planned for deployment. Trace the affected code paths and exercise the compiled core against disposable stores. Check the visible replies, verified action results, persistent records, and recovery across a restart.
- Use the GitHub workflow as an additional regression signal. Keep the candidate on the release branch pending signoff.

## 2. Preflight the target host before activation

Run these on the target Linux machine, not in CI:

```sh
bash /home/x/R2_Home/R2/tools/preflight_r2_hardware.sh
```

Resolve every `[FAIL]`. Review each `[WARN]` and confirm whether the feature is optional or required for this installation. The preflight is intended to be non-destructive; it does not install packages, migrate databases, or start/stop R2.

For the interactive desktop launcher, keep the isolated `r2-ollama.service` on `127.0.0.1:11435` stopped. Interactive R2 uses the desktop Ollama endpoint on `127.0.0.1:11434`; both servers share the GTX 1050 Ti's VRAM, so a runner loaded by the isolated service can make the interactive endpoint's `ollama ps` look empty while still consuming GPU memory. If preflight reports a `llama-server` process with no model listed, inspect `systemctl status ollama.service r2-ollama.service` and identify the owning process before stopping anything; do not kill a runner blindly.

Run database acceptance against disposable stores or an isolated copy. Identify the detached copy explicitly:

```sh
python3 tools/audit_r2_databases.py --root /path/to/isolated-r2-data
```

Keep the originals stopped while preparing the quiescent backup for final deployment. The overnight installer creates and audits a private SQLite snapshot before launching the core. Keep a separate backup of the original conversation and diary files, which the SQLite snapshot does not copy. Investigate integrity failures or unexpected schema and row reports before going ahead.

## 3. Check model and devices independently before installing R2

Use the standalone Ollama preflight for text, synthetic image inference, and GPU placement. Use host tools to check the microphone, camera, speaker, and TV/VCR or Game Boy dependencies directly. Keep the R2 executable stopped throughout these checks.

The model-copy step validates the Ollama model reference and manifest structure before writing, rejects symlinked source manifests/blob files and source/destination parent components, checks every blob digest and source blob hash, verifies copied content, and uses exclusively-created temporary files for atomic replacement. The model preflight then requires the model to retrieve an exact code from an earlier synthetic conversation turn. The probe requires a complete assistant reply with thinking disabled. A generation-limit stop or a thinking-only response fails the probe. It also runs a camera-sized 640x480 synthetic-image completion in JSON mode that must identify a known blue square, using the same context-dependent output cap as Visual.c; image-bearing inference failure/OOM blocks startup and triggers the same conservative default-only fallback as text failure.

On the 4 GiB GTX 1050 Ti, the interactive launcher and overnight runner first probe the default 20 GPU layers and 8192 context tokens. With both settings selected automatically, they retry 4096 and 2048 context at the initial layer count, then try 12, 8, and 4 layers with 8192, 4096, and 2048 context at each count. If the operator sets context explicitly, retries keep that context. An explicit GPU-layer setting disables these automatic layer retries. The separate privileged installer's initial private-server probe uses a single 12-layer / 4096-context fallback before starting the service. A successful GTX 1050 Ti preflight must confirm both real generation and mixed CPU/GPU placement.

**Trusted bootstrap requirement:** the repository now includes `tools/r2_install_root_bootstrap.sh`, which is intended to be installed once as the root-owned `/usr/local/sbin/r2-3po-install`. It contains the systemd unit definitions and does not execute or install privileged unit content from `R2_SOURCE`; the mutable model copier, database audit, preflight, and build all run as `r2`. `tools/install_r2_overnight.sh` is only a nonprivileged notice and must not be run with `sudo`. The bootstrap must be fetched from immutable commit `5ea1eed350a178cc6ad984e9c1dcaf0a7c56b405` into `/home/x` (outside the runtime-writable tree), verified against Git blob SHA `9c34c9987a7ee023f020a265a9f1da2a9df8f66e`, and only then installed as `/usr/local/sbin/r2-3po-install`. Never install it directly from `/home/x/R2_Home/R2`, where `r2` can change it between review and privileged copy. The trust boundary is addressed only when the verified root-owned bootstrap has actually been installed and checked for root ownership and non-writable group/world permissions. Do not run the mutable source-tree installer as root.

**Never run the interactive launcher and the overnight systemd core simultaneously.** They share persistent databases and are designed for one active R2 core.

## 4. Authorize the first production launch

After signoff, follow the [overnight service guide](overnight-service.md) to install the reviewed bootstrap. Invoke the protected installer with `--activate-reviewed` only when the reviewed source and live-data snapshot are ready for production. This starts R2 on the live workspace. Monitor normal operation and its journal from this point forward.

If startup fails, keep R2 stopped and disabled. Preserve the live databases, snapshot, and logs. Review any recovery using isolated copies before authorizing another production launch.

## 5. Rollback posture

- Preserve the last known-good executable and the database snapshots created before rollout.
- Do not run schema downgrade scripts or restore a database over a live process.
- If model preflight, integrity checks, or conversation tests fail, stop activation, capture logs, and investigate before retrying.
- Keep changes on this release branch until repository review, isolated runtime tests, and standalone target-host checks all pass.
