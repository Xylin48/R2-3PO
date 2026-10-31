# R2-3PO manual audit log

This log records source-level findings and their verification status. A green workflow is evidence for the listed regressions, not proof of live model or hardware behavior.

## Audit batch: 2026-10-18 — conversation response contract

### R2-AUD-001 — Incomplete or wrong-role Ollama response could be treated as an answer

- **Area:** `r2.c`, non-streaming `/api/chat` response parsing.
- **Finding:** the parser copied `message.content` without requiring a string `message.role` equal to `assistant` or a boolean `done` equal to `true`. A syntactically valid but incomplete or malformed response could therefore enter the user-facing conversation as though it were a finished answer.
- **Fix:** accept only the expected JSON types, validate the assistant/completion contract, emit a structured error event on rejection, and return failure so the existing conversation retry/fallback path handles it visibly.
- **Regression coverage:** the production predicate is exercised by `tests/r2_reply_misclassification_smoke.c` for a valid assistant completion, a wrong role, an unfinished completion, and missing metadata. `tests/ollama_request_config_smoke.py` checks the parser's JSON-type validation and fail-closed path.
- **Status:** implemented and covered by the full workflow on commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps passed). Target-host behavior is not yet verified.

### R2-AUD-002 — GPU offload regression test had drifted from the vision implementation

- **Area:** `tests/ollama_gpu_offload_smoke.py` versus `Visual.c`.
- **Finding:** the test required the old direct `json_object_object_add(options, "num_gpu", ...)` expression, although `Visual.c` had already moved to a checked, transactional JSON insertion helper. The failure was in a stale source assertion, not evidence that vision omitted the GPU setting.
- **Fix:** update the assertion to require the checked helper and the actual `num_gpu` value construction, independent of the source's line wrapping.
- **Status:** corrected and covered by the full workflow on commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps passed).


### R2-AUD-003 — Overnight installer test expected the retired systemd probe form

- **Area:** `tests/r2_overnight_service_smoke.py` versus `tools/r2_install_root_bootstrap.sh`.
- **Finding:** the regression test still required `systemctl is-active --quiet` / `is-enabled --quiet`, but the bootstrap intentionally captures the full service states to distinguish inactive, failed, unknown, masked, static, and other legitimate states before changing anything.
- **Fix:** assert the current captured-state commands and their active/enabled classification branches rather than the retired probe syntax.
- **Status:** corrected; full workflow validation is pending for the current branch head.


### R2-AUD-004 — Overnight installer notice was not executable in the Git tree

- **Area:** Git file mode for `tools/install_r2_overnight.sh`.
- **Finding:** the source-tree notice had a valid Bash shebang and content, but its committed mode was `100644`; the permission regression test correctly rejected it because the documented direct invocation requires an executable file.
- **Fix:** restored the Git tree mode to `100755` without changing the script contents.
- **Status:** mode corrected in commit `501e101279622310d82ae5c0184d50aba73b7580`; full workflow passed on descendant commit `0159635cd31b0b1b2a5dfd49a90846d2b7b59e1c` (87/87 steps).



### R2-AUD-005 — Physical money balance replacement is not failure-atomic

- **Area:** `Reality.c`, `money_set_dir_balance()`, used by cash receipt, deposit/withdrawal, purchase, and migration paths.
- **Finding:** the former implementation removed authoritative bills before replacement files were safely available. A write failure could therefore destroy the prior balance.
- **Fix:** replacement files are staged before the authoritative set moves. Phase transitions are written through a temporary marker and atomic rename; transaction and money directories are synced at key boundaries. Backups are retained whenever rollback cannot be completed. Startup recovery distinguishes partial backup movement, partial installation, and committed replacement.
- **Regression coverage:** deterministic staging failure preserves the original $5 balance; a failure after the first backup move exercises synchronous rollback; a simulated restart during installation verifies the original authoritative bills are restored.
- **Status:** source and regressions passed both complete workflows on code commit `9fe05d0e000243b4b1ba9b515c39f68259e287bd` (87/87 steps each; push [39187194766](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39187194766), pull request [39187198480](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39187198480)). Actual host power-loss/filesystem durability remains unverified.

### R2-AUD-006 — Wallet-to-piggybank transfers were not recoverable as one operation

- **Area:** `Reality.c`, `money_transfer()`, covering deposits and withdrawals.
- **Finding:** wallet and piggybank each had an individually recoverable file replacement, but the overall transfer changed two separate directories sequentially. A crash after the first replacement and before the second could leave the transfer half-applied.
- **Fix:** added a durable SQLite transfer journal recording both old and proposed balances before either physical balance changes. A pending journal restores both old balances on synchronous failure or startup; a committed journal keeps the completed transfer and can be cleaned on a later start. The journal remains until recovery/cleanup succeeds.
- **Regression coverage:** deterministic failure after wallet replacement verifies both balances return to their original values; a restart fixture inserts a pending journal and partial new physical balances, then verifies startup restores the original $5 cash/$0 bank state.
- **Status:** implementation and regression passed both complete workflows on code commit `9fe05d0e000243b4b1ba9b515c39f68259e287bd` (87/87 steps each). Target-host power-loss behavior remains unverified.

### R2-AUD-007 — Purchase money and inventory persistence are not crash-atomic

- **Area:** `Reality.c`, `r2_reality_buy_item()`, spanning physical Wallet/piggybank files and Reality/fridge inventory storage.
- **Finding:** the purchase first persists the money deduction, then separately calls `r2_reality_add_item()`. Synchronous inventory failure attempts to restore the old balance, but a process crash after the deduction and before the item insertion can leave R2 charged without receiving the item. The transfer journal added for deposits/withdrawals does not yet cover purchases.
- **Fix:** added an additive `r2_purchase_journal` table recording old/proposed cash and bank balances, the intended item/container, pre-purchase quantity, and phase before any physical deduction. Startup recovery runs after both Reality and fridge databases are available. It checks whether the intended inventory stack advanced in the intended container: if not, it restores both old physical balances; if the item persisted, it retains the charge and reconciles the physical balances to the proposed values. The journal is cleared only after reconciliation or successful completion.
- **Concurrency/lifecycle fix:** a dedicated purchase mutex serializes the entire cross-database purchase, all other money mutations, and Reality shutdown. This prevents a concurrent deposit/withdrawal or shutdown from treating an active purchase as abandoned or closing its databases mid-commit. The committed journal is retained until the physical balances have been re-read successfully.
- **Regression coverage:** `tests/money_wallet_smoke.c` injects interruption before item persistence and after item persistence for both regular pocket inventory and the separate fridge database. Restart recovery verifies refund when no item was delivered and preserves the charge when the item exists. The tests also verify that the remaining money and inventory fixtures remain correct for subsequent purchases.
- **Status:** implemented and passed both complete workflows on code commit `d939b13af6e78441e2f9e967deb2e2b92be3dcad` (87/87 steps each): push [39188485522](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39188485522) and pull request [39188481756](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39188481756). These are deterministic interruption/restart tests, not physical power-loss tests. Target-host filesystem durability remains unverified.

### R2-AUD-008 — Thread-creation failure can leave initialized subsystems open

- **Area:** `r2.c`, `r2_init()` failure paths after Reality, Eyes, Ears, Visual, Life Log, Reward, Sensory Journal, Alternate-Self, and Imagination initialization.
- **Finding:** if creating the Hands worker or autonomous-thinking thread fails, the failure paths stopped/joined what they needed but only shut down the diary and closed the main memory database before returning `-1`. Other initialized subsystem handles/state could remain live. The normal `main()` path returned immediately when `r2_init()` failed instead of calling `r2_shutdown()`.
- **Fix:** both startup thread-creation failures now return through one cleanup owner: `main()` calls `r2_shutdown()` when initialization fails. The autonomous-thinking creation failure still stops and joins the already-started Hands worker before returning; central shutdown then closes Reality, Eyes/Ears, Visual, Reward, Life Log, Sensory Journal, diary, memory DB, and other initialized runtime state in normal shutdown order. Production behavior still uses `pthread_create`; deterministic environment-driven failure injection is compiled only with `R2_TEST_STARTUP_FAILURE_INJECTION`.
- **Regression coverage:** `tests/startup_failure_unwind_smoke.py` verifies both injected failure points route through centralized cleanup and that the production build cannot activate the test hooks. The workflow runs this regression and compiles R2 with the launcher-equivalent sources/flags. This source-level regression does not substitute for a target-host fault-injection run.
- **Status:** fixed and validated on source commit `908f05aa7850e2cd479f02e2a391886efbcc2c33`. Both complete workflows passed (87/87 steps each): push [39192095002](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39192095002) and pull request [39192098821](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39192098821). The current audit-log update is documentation-only; the resulting exact final head is being revalidated. Target-host fault-injection remains unverified.

## Manual source-review checkpoint: conversation and memory pipeline

Reviewed the current implementations and call flow in `r2.c`, `r2.h`, `R2Reply.c`, `Visual.c`, and the production-backed conversation regression tests.

- The foreground conversation lock spans the whole turn, preventing interleaved user/assistant turns and failed-turn rollback from removing another caller's newer messages.
- The active user message is preserved as a real user-role message; retrieval is assembled separately as historical-evidence system context. The payload builder reserves the authoritative user turn and refuses inference if that turn cannot be serialized in full.
- Retrieved memories and Life Log/diary/visual evidence are bounded before prompt insertion. The short-lived cache stores only generic persistent-memory retrieval; current Reality and live visual observations are refreshed separately.
- The shared Ollama gate gives foreground text inference priority over opportunistic background work and vision requests. Tool results are tagged as internal and are not confused with a fresh human user turn.
- The reply sanitizer removes private reasoning blocks, fails closed for unclosed private blocks, and preserves/redacts tag syntax in code examples. Its tests cover nested/crossing tags and malformed truncation.
- The post-turn bookkeeping is scheduled after user-facing delivery; worker-start failure is logged instead of blocking the response.

## Manual source-review checkpoint: tool routing, filesystem tools, and persistence

- Traced all three background command dispatch paths through `start_background_task()`; fixed false-success reporting and added a regression.
- Traced the Game Boy control-plane response contract against `GameBoyAdvance.py`; fixed fail-open JSON handling and added a regression.
- Reviewed filesystem tool path resolution: workspace paths are canonicalized and checked for workspace containment; fenced/HTML code examples are excluded by the control-marker parser. This pass did not establish a reproducible path-escape defect, though filesystem writes are not a full descriptor-relative defense against a hostile concurrent symlink swap.
- Traced Reality add/move/remove paths and the existing money/purchase journals. The separate fridge/pockets crash window was fixed under R2-AUD-012 with a durable snapshot journal and restart-injection tests.
- Continued the startup/shutdown review and fixed direct `r2_init()` fatal exits so required initialization failures route through centralized cleanup, with state-aware libcurl and diary cleanup.

This remains a bounded manual review, not a claim that every subsystem has been audited. The latest-head full workflow passed on commit `7ba1cdac72d0e244f19db1de831b398ed4f40dbc` (90/90 steps; run [39197843416](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39197843416)). Target-host acceptance remains open.

## Release gates still open

- Full workflow passed on code/test commit `cc375fe440407ef87b7576e2fe7a4840994c6218` (90/90 steps; run [39198701049](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39198701049)). It includes the tool-result allocation-failure warning contract and direct no-retry response. This audit-log update is documentation-only; the target-machine acceptance gates below remain open.
- On the target machine, verify real Qwen 3.5 9B multi-turn chat, retry/fallback behavior, measured CPU/GPU placement and RAM headroom, and connected devices. CI cannot perform these checks.
- Before installation, verify that the trusted root bootstrap is installed at `/usr/local/sbin/r2-3po-install`, root-owned, and not group/world-writable. The documented immutable source commit and Git blob SHA are checked against the current branch; this repository check does not prove the host has installed that copy.
- Keep PR #161 draft and `main` untouched until repository validation and target-host acceptance are complete.

### R2-AUD-009 — Fatal exits inside `r2_init()` bypass the centralized unwind path

- **Area:** `r2.c`, early and mid-stage failures inside `r2_init()`.
- **Finding:** multiple initialization failures still call the process-wide `die()` helper, which calls `exit(EXIT_FAILURE)`. This bypasses `main()`'s new `r2_init() != 0` cleanup path. Several of these branches perform hand-written partial cleanup first, making a blind conversion to `return -1` unsafe: the shutdown guard currently checks only `core_initialized`, `db`, `eyes`, and `ears`, so it can skip cleanup after a branch has already nulled those pointers while other subsystems or sound state may still be initialized.
- **Impact:** OS process teardown will close ordinary process-owned descriptors, but R2's own orderly subsystem shutdown, consistent failure reporting, and in-process retry/unwind contract are not guaranteed for these startup failures.
- **Fix:** required failures in `r2_init()` now report an error and return to `main()`'s centralized cleanup owner instead of calling process-wide `exit()`. An explicit init-attempt flag makes cleanup run even if an earlier branch already nulled the core handles; libcurl cleanup is gated on successful initialization; the diary handle is closed when its initializer fails after opening it. Unrelated runtime `die()` behavior is unchanged.
- **Regression coverage:** expanded `tests/startup_failure_unwind_smoke.py` to check direct init exits are gone, early partial initialization reaches central cleanup, and libcurl/diary cleanup are state-aware. Existing thread-creation failure checks remain.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). The smoke test checks source contracts and existing thread-failure regressions; it does not inject every initialization failure at runtime.

### R2-AUD-010 — Background command dispatch could report success when no task was queued

- **Area:** `r2.c`, structured `[HAND_REQUEST]`, explicit `[RUN]`, and direct `[HAND]` tool dispatch.
- **Finding:** `start_background_task()` returns `NULL` when allocation fails or shutdown has already begun. All three callers previously printed “Background task successfully started” anyway, with an empty task ID, which could tell R2 and the user that a command was running when no work had entered the queue.
- **Fix:** each caller now branches on the returned task ID and emits an explicit enqueue-failure result when the queue operation fails. It does not claim execution or invent a task ID.
- **Regression coverage:** added `tests/hand_enqueue_failure_smoke.py` and wired it into `.github/workflows/build.yml` to check all three dispatch sites.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; the regression passed and the full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). No target-machine execution is implied.

### R2-AUD-011 — Game Boy command output could be accepted without a valid success response

- **Area:** `r2.c`, Game Boy Advance command bridge and JSON response parsing.
- **Finding:** the bridge defaulted a missing or unparsable `ok` field to true. Because `gameboy_json_bool()` also accepted non-boolean JSON values, a zero exit code with malformed output or an invalid `ok` value could be treated as a successful action. That could produce a false success response or record an action that the bridge had not verified.
- **Fix:** JSON boolean fields now require the actual JSON boolean type. The command bridge accepts the documented array shape for `list`, the object shape for `status`, and requires an explicit boolean `ok: true` for action responses; process exit code alone is insufficient.
- **Regression coverage:** added `tests/gameboy_json_contract_smoke.py` and wired it into `.github/workflows/build.yml`. This is a source-contract regression, not a live emulator/hardware test.
- **Status:** fixed and validated on commit `d9c1ff5e3d4f4d48f735ee03374260fde3d0e503`; the response-contract regression, strict build, and full workflow [39194830081](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39194830081) passed (89/89 steps). This is not a live emulator/hardware test.

### R2-AUD-012 — Cross-database fridge transfers lacked crash recovery

- **Area:** `Reality.c`, moving a tracked stack into the separate fridge database and taking food from the fridge into pockets.
- **Finding:** both directions updated the fridge database and Reality database in separate operations with only in-process rollback. A process crash between writes could duplicate a stored item or lose a taken food item.
- **Fix:** added backward-compatible `r2_inventory_transfer_journal` in the Reality database. Before either side changes, it persists a snapshot of the prior Reality object, item-memory high-water mark, and fridge row metadata/quantity. Meal transfers also snapshot modeled needs, the 72-hour starvation marker, and food-experience high-water mark. Both transfer directions mark the journal committed only after both database changes succeed. Startup and subsequent transfer/purchase paths recover a pending journal by restoring both databases and mirrors; committed journals are cleared without undoing a completed transfer. Meal entry recovers any retained journal before advancing the clock, preventing a later rollback from erasing newly elapsed time. Interrupted recovery is recorded in the factual log.
- **Regression coverage:** extended `tests/reality_needs_smoke.c` with test-only crash injection after the fridge write in each direction. The test restarts Reality and checks both authoritative databases, prior fridge metadata, and journal clearance.
- **Validation:** latest-head workflow [39197843416](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39197843416) passed all 90/90 steps. This includes strict production compilation, the Reality needs smoke test with crash-injected fridge-to-pocket, room-to-fridge, and fridge-meal recovery, startup-thread failure cleanup, and isolated overnight SIGTERM lifecycle integration. CI still does not prove behavior on the user's physical machine.



### R2-AUD-013 — Tool-result allocation failure could hide a completed side effect

- **Area:** `r2.c`, `process_tools_with_status()` result-buffer growth.
- **Finding:** the `APPEND` macro returned `NULL` if `realloc(results, cap)` failed. That allocation can happen after a tool has already changed persistent state. The caller would then receive a generic tool-processing failure instead of a warning that an action may have completed, making a manual retry risky.
- **Fix:** on result-buffer growth failure, preserve the existing allocation and return an explicit warning that processing had begun, an action may already have completed, and R2 will not repeat it automatically. Returning from the tool processor also stops subsequent tool markers from executing.
- **Regression coverage:** extended `tests/conversation_recovery_smoke.py` to check this allocation-failure contract. This is a source-contract regression; the failure is not injected at runtime.
- **Status:** fixed and validated on code/test commit `cc375fe440407ef87b7576e2fe7a4840994c6218`; full workflow [39198701049](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39198701049) passed all 90/90 steps, including the updated conversation-recovery regression and strict production build. The allocation-failure branch is protected by a source-contract test, not an injected runtime OOM test.

### R2-AUD-014 — Ollama readiness and vision paths accepted incomplete model responses

- **Area:** `tools/preflight_ollama_model.py` text/image readiness checks; `Visual.c` live visual inference response parsing.
- **Finding:** the core conversation path requires an Ollama response to contain `message.role=assistant` and boolean `done=true`, but the startup text/vision preflight and live Visual subsystem accepted non-empty `message.content` without checking that completion contract. A partial or malformed response could therefore be treated as a successful readiness probe or visual description.
- **Fix:** preflight now rejects text and image replies unless the assistant role and explicit boolean completion flag are present. Visual inference applies the same contract and logs `vision_invalid_completion_contract` when it rejects a malformed response.
- **Regression coverage:** extended `tests/ollama_preflight_smoke.py` with incomplete text/image response cases and extended `tests/vision_watch_cancellation_smoke.py` to guard the live vision contract.
- **Status:** fixed and validated on the release candidate tested by GitHub Actions run [4847](https://github.com/Xylin-Holstein/R2-3PO/actions/runs/39204350398), which passed all 90/90 steps, including the offline Ollama preflight regression. This does not replace target-machine model or camera testing.

### R2-AUD-015 — Control-only model replies could pass the visible-answer gate

- **Area:** `r2.c`, `reply_has_visible_text()` before `process_tools()`.
- **Finding:** the visibility predicate only checked for non-whitespace bytes. A reply containing only a command block such as `[RUN]... [END RUN]` or a one-line action marker such as `[WORLD]` was therefore treated as a visible answer, despite the surrounding recovery logic intending to retry marker-only output before any action executes.
- **Fix:** the visibility check now ignores recognized action-control blocks and command lines, and fails closed on an unterminated control block. A reply must contain actual non-control text before tool processing is allowed.
- **Regression coverage:** extended `tests/conversation_recovery_smoke.py` to guard the control-marker filtering and its placement before action execution, and added runtime assertions to `tests/recent_context_budget_runtime.c` for marker-only, malformed, and ordinary replies.
- **Status:** fixed and validated on release-candidate workflow [4890](https://github.com/Xylin48/R2-3PO/actions/runs/39216933930), which passed all 90/90 steps, including the runtime reply-visibility checks, strict production build, and overnight lifecycle integration. No target-machine execution is implied.

### R2-AUD-016 — Tool-result OOM warning could overrun the original allocation

- **Area:** `r2.c`, `process_tools_with_status()`, tool-result buffer growth failure.
- **Finding:** the growth loop doubled `cap` in place before calling `realloc()`. If `realloc()` failed, the existing `results` allocation still had its original size, but the failure warning used the already-increased `cap` as the `snprintf()` bound. Under memory pressure, this could write beyond the original allocation.
- **Fix:** compute the desired capacity in a separate `_new_cap` variable; leave `cap` unchanged until `realloc()` succeeds. The warning is now formatted with the actual original allocation size. Capacity doubling also checks against `SIZE_MAX` before multiplying.
- **Regression coverage:** updated `tests/conversation_recovery_smoke.py` to assert the failure path formats the warning using the original `cap`, does not commit `_new_cap` before successful reallocation, and updates `cap` only after adopting the new buffer.
- **Status:** fix and regression committed on the release branch and validated in full workflow [4890](https://github.com/Xylin48/R2-3PO/actions/runs/39216933930), which passed all 90/90 steps, including the action-result OOM contract, strict production build, and overnight lifecycle integration. No target-machine execution is implied.

### R2-AUD-017 — Fridge context copied beyond the formatted line buffer

- **Area:** `Reality.c`, `r2_fridge_list()`.
- **Finding:** `snprintf(line, sizeof(line), ...)` returns the number of bytes that would have been written, which can exceed `sizeof(line)-1` when a stored item name or description is unusually long. The old code used that required length directly in `memcpy(out + len, line, add)`, reading past the 2,048-byte stack buffer.
- **Fix:** clamp copied bytes to the bytes actually stored in `line`, mark truncated entries with an explicit suffix, and guard the output-length addition before allocation/copy.
- **Regression coverage:** extended `tests/fridge_context_smoke.py` to require the truncation clamp and overflow guard before the copy, and extended `tests/fridge_legacy_schema_smoke.c` to insert a 6 KB description and exercise the production fridge-context formatter at runtime.
- **Status:** production fix and regressions are committed on the release branch. The first runtime CI attempt caught a test-fixture issue—not a formatter failure—because the legacy table's `qty` and `amount` columns remain `NOT NULL`; the fixture now supplies both aliases. The next run caught a static-test escaping mistake; the assertion now checks the literal backslash-n spelling in the C source. The corrected candidate passed all 90/90 steps in workflow [4890](https://github.com/Xylin48/R2-3PO/actions/runs/39216933930), including the 6 KB-description runtime regression, fridge-context contract, strict production build, and overnight lifecycle integration. No target-machine execution is implied.

### R2-AUD-018 — Physical change-file parsing used an out-of-range scanf conversion

- **Area:** `Reality.c`, `money_change_cents()`.
- **Finding:** parsing `change.txt` with `fscanf(..., "%ld", ...)` can invoke undefined behavior when a corrupted or manually edited file contains an integer outside the range of `long`. The old `lstat()` followed by `fopen()` also left a symlink replacement window between checking and opening the authoritative file.
- **Fix:** open with `O_NOFOLLOW | O_CLOEXEC`, validate the opened descriptor with `fstat()`, read at most a small fixed buffer, parse with `strtol()` and range checks, and reject oversized input, trailing junk, embedded NULs, or non-regular files. A genuinely missing file still means zero cents, preserving the wallet's existing semantics.
- **Regression coverage:** extended `tests/money_wallet_smoke.c` to place an integer far beyond `LONG_MAX` and an embedded-NUL payload in the physical change file, verifying that money operations fail closed without changing the authoritative bill count.
- **Status:** fix and runtime regressions are committed on the release branch and passed all 90/90 steps in workflow [4890](https://github.com/Xylin48/R2-3PO/actions/runs/39216933930), including the malformed-change-file runtime checks, strict production build, and overnight lifecycle integration. No target-machine execution is implied.

### R2-AUD-019 — Alternate-Self comparison used an overflow-prone scanf conversion

- **Area:** `shell.c`, `shell_alternate()`, `alternate compare` command.
- **Finding:** comparison IDs were parsed with `sscanf("%lld %lld %c", ...)`. An out-of-range decimal supplied as a command argument can trigger undefined behavior in a scanf numeric conversion, and the parser contract was harder to audit than the checked parser already used by the adjacent commands.
- **Fix:** added `shell_parse_pair_ids()` using `strtoll()`, explicit `errno`/range checks, required whitespace between the two IDs, and rejection of missing values or trailing tokens.
- **Regression coverage:** extended `tests/build_configuration_smoke.py` to require the comparison route to use the checked parser and verify its overflow/trailing-token guards.
- **Status:** fix and regression are committed and validated at branch head `b27f39c6be029930be7024f4fd999ac2959649ac`; full workflow [4918](https://github.com/Xylin48/R2-3PO/actions/runs/39219321852) passed all 90/90 steps, including the checked comparison-ID parser regression and strict production build. No target-machine execution is implied.

### R2-AUD-020 — Model-generated Alternate-Self markers used overflow-prone ID parsing

- **Area:** `r2.c`, `process_tools_with_status()`, `[ALTERNATE_SHOW]`, `[ALTERNATE_COMPARE]`, `[ALTERNATE_RETAIN]`, and `[ALTERNATE_DISCARD]`.
- **Finding:** these model-generated control markers used `sscanf()` with `%lld` conversions. A malformed or out-of-range generated ID could invoke undefined behavior before the marker was rejected.
- **Fix:** added line-bounded `strtoll()` parsers for a single positive ID and an exact pair of positive IDs. They reject numeric overflow, missing IDs, non-positive values, trailing junk, and extra tokens.
- **Regression coverage:** extended `tests/recent_context_budget_runtime.c` to exercise valid IDs, overflow values, negative IDs, trailing junk, extra tokens, and missing second IDs against the production helpers.
- **Status:** fix and runtime regressions are committed and validated at branch head `b27f39c6be029930be7024f4fd999ac2959649ac`; full workflow [4918](https://github.com/Xylin48/R2-3PO/actions/runs/39219321852) passed all 90/90 steps, including valid/invalid model-generated Alternate-Self ID cases and strict production compilation. No target-machine execution is implied.

### R2-AUD-021 — Concurrent visual analyses shared one focus-region temporary file

- **Area:** `Visual.c`, `publish_focus_region()`.
- **Finding:** the visual analyzer can perform model inference outside the database mutex, but every focus update used the same `focus.tmp` path. Overlapping analyses could truncate, rename, or unlink one another's pending focus update, leaving the display overlay stale or missing.
- **Fix:** serialize the complete focus publication path with a dedicated mutex; the model inference and database work remain outside that lock.
- **Regression coverage:** `tests/vwebcam_smoke.py` now asserts that all focus updates pass through the serialized wrapper and that the wrapper unlocks after the implementation returns.
- **Validation:** branch head `b27f39c6be029930be7024f4fd999ac2959649ac` passed all 90/90 steps in [workflow 4918](https://github.com/Xylin48/R2-3PO/actions/runs/39219321852), including the visual JSON ownership smoke, focus-publication serialization guard, strict production compilation, and overnight shutdown integration. Target-machine display/camera acceptance is still separate.

### R2-AUD-022 — Trusted bootstrap download instructions retained the former GitHub owner path

- **Area:** `docs/overnight-service.md`, immutable download instructions for `tools/r2_install_root_bootstrap.sh`.
- **Finding:** the installation procedure still downloaded from the former `Xylin-Holstein` raw-content namespace after the repository account changed to `Xylin48`. The pinned old-owner URL did not resolve to file content through the GitHub retrieval path, so a clean install could fail before hash verification.
- **Fix:** changed only the documented raw-content URL to the current `Xylin48/R2-3PO` namespace. The immutable commit and expected Git blob SHA remain unchanged.
- **Validation:** fetched the bootstrap from the new raw-content URL at immutable commit `2aec6cee84ed1274a6c7324ec6ac5560d8088240`; GitHub reported blob SHA `6e9906e6ac3e7299b368585fc7e842f32ceb2646`, exactly matching the documented and installer-enforced expected SHA. The old URL failed to return file content. No trusted bootstrap code or runtime behavior was changed.

### R2-AUD-023 — Startup thread-creation cleanup was only source-checked

- **Area:** `r2.c` startup lifecycle and `.github/workflows/build.yml`.
- **Finding:** the existing regression inspected both thread-creation failure branches and verified that `main()` calls centralized shutdown, but it did not execute the partial-startup cleanup path.
- **Fix/test:** added `tests/startup_failure_unwind_runtime.c`, compiled `r2.c` with the test-only `R2_TEST_STARTUP_FAILURE_INJECTION` flag, and ran separate isolated processes for failed Hands-thread creation and failed autonomous-thinking/diary-thread creation. Each case requires `r2_init()` to fail and then calls `r2_shutdown()`; the production build remains without active injection hooks. The existing source-contract test now asserts the runtime harness is wired into CI.
- **Validation:** exact code/test head `ed013aac011f0ea238ab56af1f92ad054c22fa40` passed the full GitHub Actions workflow, including both runtime fault-injection cases and the strict production compile: [workflow run 39229363923](https://github.com/Xylin48/R2-3PO/actions/runs/39229363923), 91/91 steps successful.
- **Scope:** this closes the identified CI coverage gap; it does not replace host-side acceptance of live Qwen inference, measured GPU/RAM behavior, persistent databases, or physical devices.



### R2-AUD-024 — deterministic feeding overrode the autonomous needs model

- **Area:** `r2.c`, `Reality.c`, `Reality.h`, and autonomous-needs tests.
- **Finding:** If the autonomous model decision failed or its context could not be built, the controller invoked a deterministic feeding routine that consumed verified food without a model-selected action. That made a programmatic fallback, rather than R2's own decision, determine the simulated behavior.
- **Fix:** Removed the deterministic feed function and its API, removed fallback calls from all controller failure paths, and made failures log and leave the modeled world unchanged. Kept authoritative inventory checks, food-metric validation, transaction rollback, and conversation-turn coordination.
- **Regression coverage:** Replaced the old safeguard source test with `tests/autonomous_needs_controller_smoke.py`; removed automatic-feed expectations from `tests/reality_needs_smoke.c`; updated conversation recovery/context integration contracts and CI workflow wiring.
- **Validation:** exact head `78f97471831f1fd9743cb25703ae6a4568eb3d6f` passed [workflow 39357732761](https://github.com/Xylin48/R2-3PO/actions/runs/39357732761), 91/91 steps. Live autonomous decisions still require target-host testing.

## Audit batch: 2026-10-27 — checked SQLite mutation paths

### R2-AUD-2026-10-27-001 — fridge decrement ignored parameter-binding failures

- **Area:** `Reality.c`, `fridge_change_one()`.
- **Severity:** High / inventory integrity.
- **Finding:** The fridge item lookup and subsequent DELETE/UPDATE ignored `bind_text()` results. A failed binding could make the operation act on an unintended/unmatched row or report a generic failure without preserving the exact SQL result. The mutation also treated `SQLITE_DONE` as sufficient without confirming exactly one row changed.
- **Fix:** Check the lookup bind before stepping; check the mutation bind before stepping; and require exactly one affected row before reporting success.
- **Regression coverage:** `tests/fridge_context_smoke.py` now asserts the production function checks both bindings and validates the affected-row count.
- **Validation status:** Passed the full repository CI workflow on code revision `7b9309d1` (92/92 steps). This is source-level/CI validation, not a live database test on the target PC.

### R2-AUD-2026-10-27-002 — Reality metric updates ignored SQLite bind failures

- **Area:** `Reality.c`, `update_hunger_locked()` and `r2_reality_sleep()`.
- **Severity:** High / modeled state and persistence integrity.
- **Finding:** Hunger/satiety/sleepiness/energy updates, elapsed-world-time persistence, and sleep recovery stepped prepared statements without checking every bind result. This could permit an incomplete update to proceed rather than propagating the database failure.
- **Fix:** Chain each bind through the SQLite result code, execute only after all binds succeed, and validate that the singleton Reality row or metadata row was actually updated where applicable.
- **Regression coverage:** `tests/subsystem_wiring_smoke.py` now checks the production hunger/time and sleep paths for checked bindings.
- **Validation status:** Passed the full repository CI workflow on code revision `7b9309d1` (92/92 steps). Source assertions supplement the runtime Reality regression; target-host persistence/restart acceptance remains outstanding.

### Current workload checkpoint

- **Working branch:** `work/r2-priority-fixes-from-audit-2026-10-25`.
- **Validation PR:** [#177](https://github.com/Xylin48/R2-3PO/pull/177), draft and unmerged, targeting the isolated integration-validation branch.
- **Main branch:** untouched.
- **Next audit pass:** continue the line-by-line review of SQLite error propagation and transaction boundaries across Reality, rewards, persistent memory/diary, and conversation handling. Review the full exact-head workflow and fix any newly confirmed regressions before closing findings.
- **Release boundary:** CI cannot verify live Qwen response quality, GTX 1050 Ti allocation, or actual database/device state on the target PC.

### R2-AUD-2026-10-27-003 — unchecked bindings across Reality tick and TV device persistence

- **Area:** `Reality.c`, persistent-time catch-up, normal tick/quantity-decay updates, TV signal lookup, and TV device registration.
- **Severity:** High / world-state consistency.
- **Finding:** Several prepared statements ignored SQLite binding results and still executed. A failed bind could make the operation report a result based on incomplete parameters.
- **Fix:** Propagate each bind result and execute the statement only when all required bindings succeed. Existing transaction rollback and affected-row checks remain in place.
- **Regression coverage:** `tests/subsystem_wiring_smoke.py` now checks Reality tick and TV-device binding paths. Existing Reality behavior tests exercise normal tick/state persistence.
- **Validation status:** Passed the full repository CI workflow on code revision `7b9309d1` (92/92 steps); target-host behavior remains unverified.

### R2-AUD-2026-10-27-004 — unchecked bindings in Life Log, memory, and reward lookups/writes

- **Area:** `Log.c` event deduplication, milestone insertion, activity session start/end, location transitions, and system snapshots; `r2.c` persistent memory timestamp update and Game Boy event deduplication; `Reward.c` active modifier lookup.
- **Severity:** High / persistence and duplicate-event integrity.
- **Finding:** Several writes and lookups stepped prepared statements after unchecked bindings. In particular, incomplete activity/location/system snapshot writes could be treated as successful, while a memory or Game Boy lookup could use incomplete query parameters.
- **Fix:** Chain each bind through the SQLite result code, step only after all required bindings succeed, and preserve existing write-success/affected-row checks.
- **Regression coverage:** `tests/subsystem_wiring_smoke.py` now asserts the updated memory, reward, and Life Log paths. Existing migration and integration tests exercise real writes against SQLite.
- **Validation status:** Passed the full repository CI workflow on code revision `7b9309d1` (92/92 steps).

### Database backward-compatibility audit checkpoint

The repository has targeted legacy-schema tests for the shared memory/Life Log database, Reality/fridge inventory, rewards, addictions, Visual Experience Library, and Game Boy's cartridge/state databases. The Game Boy regression now covers migration from the old combined `gameboy.db` layout into `State/console_state.db`, including ID collisions, preservation of both event histories, retained source data, and new writes. Sensory Journal and Alternate-Self use tables within the shared `r2_memory.db`; their additive migration paths were source-reviewed and their normal persistence paths are exercised by the suite, but dedicated old-column fixtures for those individual tables remain a possible coverage improvement. The full CI workflow passed 92/92 steps on code revision `7b9309d1`. This establishes repository-level compatibility for the exercised schemas, not a claim that the user's on-disk databases have been inspected or that target-host migration has run.

### R2-AUD-2026-10-28-005 — legacy Game Boy state could be stranded in the old combined database

- **Area:** `GameBoyAdvance.py`, `gameboy.db` and `State/console_state.db`.
- **Severity:** High / device-state continuity.
- **Finding:** Current code stores cartridge state in `gameboy.db` and console/session state in `State/console_state.db`, but the repository's own module documentation described the earlier combined layout. Without an import, old console state, event history, and cartridge inventory tables in `gameboy.db` could be left unread by the new runtime.
- **Fix:** Add a transactional, idempotent, additive import for legacy `console_state`, `console_events`, and `cartridge_inventory` tables from the attached legacy database. Preserve an existing state DB's row when IDs conflict, retain both event histories if event IDs conflict, keep the original source tables intact, and only mark the migration when legacy tables actually exist.
- **Regression coverage:** Added `tests/gameboy_legacy_schema_smoke.py` and wired it into CI. It exercises old schemas in both files, preserves existing rows, verifies post-migration writes and SQLite integrity, confirms the source data remains readable, and checks that a fresh install is not prematurely marked migrated.
- **Validation status:** The new exact-head CI run must pass before this migration is considered verified. No live hardware or mGBA session was available for this repository-side audit.


### Validation result: 2026-10-28

- **Exact code revision tested:** `7b9309d1b83cf5703408cedcaa723b98ac70328f`.
- **GitHub Actions:** [run 40629395737](https://github.com/Xylin48/R2-3PO/actions/runs/40629395737) — passed, 92/92 steps.
- **What this proves:** strict build and the configured regression suite, including the primary legacy-schema tests and the new Game Boy split-database migration test.
- **What this does not prove:** live Qwen/Ollama response quality on the target PC, actual GTX 1050 Ti behavior, or the integrity/schema of the user's real database files. Those require target-host checks and a read-only database audit before any migration is applied to user data.

### R2-AUD-2026-10-28-006 — Alternate-Self writes assumed timestamp defaults in older schemas

- **Area:** `AlternateSelf.c`, `r2_alternate_self_branches` and `r2_alternate_self_event_links`.
- **Severity:** High / persistent hypothetical-history continuity.
- **Finding:** Branch and link inserts relied on `created_utc` defaults. An older table can have the timestamp columns declared `NOT NULL` without a default, causing writes to fail after additive migration.
- **Fix:** Branch creation and hypothesis import now explicitly supply `CURRENT_TIMESTAMP` for `created_utc` and `updated_utc`; event-link insertion explicitly supplies `created_utc`.
- **Regression coverage:** `tests/alternate_self_smoke.c` now starts with a legacy branch table and legacy link table whose timestamp columns have no defaults, verifies the old branch remains readable, and exercises new branch/link/import writes.
- **Validation:** Passed the full repository CI workflow on code revision `5e0e5018` (92/92 steps).

### R2-AUD-2026-10-28-007 — Sensory Journal writes could be ignored by legacy timestamp constraints

- **Area:** `SensoryJournal.c`, journal indexing and diary-link backfill/runtime writes.
- **Severity:** High / sensory-history continuity.
- **Finding:** Backfill and runtime inserts omitted `indexed_utc` or `linked_utc`. If an older table already had either column as `NOT NULL` without a default, `INSERT OR IGNORE` could silently skip the row.
- **Fix:** All backfill and runtime index/link inserts now provide `CURRENT_TIMESTAMP` explicitly, independent of schema defaults.
- **Regression coverage:** `tests/sensory_journal_search_smoke.c` now exercises pre-existing legacy timestamp columns without defaults, backfills a prior diary event, verifies non-empty timestamps, and checks that new diary events remain indexed and linked after restart.
- **Validation:** Passed the full repository CI workflow on code revision `5e0e5018` (92/92 steps).

### Latest code validation checkpoint

- **Exact tested code revision:** `5e0e5018aedcd624c2284366198df35cc96920d2`.
- **GitHub Actions:** [run 40630710375](https://github.com/Xylin48/R2-3PO/actions/runs/40630710375) — passed, 92/92 steps.
- **Repository-level status:** All configured strict build/regression steps pass, including the primary legacy database tests, Game Boy split-database import, Alternate-Self legacy timestamp constraints, and Sensory Journal legacy timestamp constraints.
- **Outstanding acceptance boundary:** The user's actual `R2_Home` databases have not been opened or audited in this session, and live Qwen/Ollama/GPU behavior has not been exercised on the target PC. Run the existing read-only preflight against the actual data directory before any on-disk migration or live acceptance.


### R2-AUD-2026-10-28-008 — Visual observation blocked capture during Qwen inference

- **Area:** `r2.c` visual watcher and the existing `Eyes.c` frame stream.
- **Finding:** the observer captured and analyzed on one worker while holding the capture lock across Qwen inference, then waited an additional fixed interval before sampling again. Slow inference could stall the FFmpeg pipe and make the next analyzed frame stale; the nominal 15-second loop was sparse sampling rather than continuous frame acquisition.
- **Fix:** split continuous Eyes capture from Qwen analysis. The capture worker keeps draining the live stream and publishes the newest frame snapshot; the analysis worker receives an owned copy outside the capture lock and analyzes successive fresh frames at the model's actual processing rate. Explicit one-frame vision requests also copy the frame and release the capture lock before inference. Each observation includes the previous successful description from the same source so frame-to-frame changes have explicit context; changing or restarting a source resets that comparison context. A failed snapshot is reported rather than silently treated as a successful observation. Shutdown joins both workers and releases the retained frame buffer.
- **Regression coverage:** added `tests/vision_watch_continuous_feed_smoke.py` to verify independent capture/analysis workers, latest-frame handoff, sequential context, source-restart reset, unlocked explicit inference, and joined shutdown. Full workflow run [40651201828](https://github.com/Xylin48/R2-3PO/actions/runs/40651201828) passed all 93/93 steps on code commit `6bb344744b7fe67e48cfd0f8929110259617c636`, including the strict production build and the new regression.
- **Scope:** the image-only Qwen model still receives individual images, not a native video tensor. Continuous acquisition plus successive frame analysis is the implemented bridge; model throughput determines observation cadence, and no claim is made that every raw source frame is individually inferred. Target-host video playback and live Qwen inference remain unverified.


## Audit batch: 2026-10-28 — autonomous sensory input and database relationships

### R2-AUD-2026-10-28-009 — Continuous Eyes input automatically interpreted every sampled frame

- **Area:** `r2.c`, `Visual.c`, Ollama message serialization, Watch coordinator.
- **Finding:** the continuous Eyes capture worker fed successive frames to a vision-analysis worker that generated descriptions and persisted interpretations without a brain-selected reason to inspect the scene. That conflated receiving sensory input with attention and analysis.
- **Fix:** continuous capture now maintains an owned latest-frame snapshot. The raw JPEG/base64 image is attached to the active user message through Ollama's `images` field when Eyes is open. The conversation brain receives the image directly and decides what to attend to; the capture worker no longer calls the vision model or creates automatic descriptions. Explicit `vision see` remains the deliberate inspection path. Explicit visual questions may open Eyes; ordinary turns do not silently open a camera. A source is closed if continuous capture cannot start.
- **Regression coverage:** `tests/vision_watch_continuous_feed_smoke.py`, `tests/vision_watch_cancellation_smoke.py`, and `tests/qwen35_model_smoke.py` cover raw-frame handoff, deliberate-only interpretation, and lifecycle. The latest full CI result is pending; this entry does not claim runtime hardware acceptance.

### R2-AUD-2026-10-28-010 — Ears and Watch automatically transcribed incoming sound

- **Area:** `r2.c`, `shell.c`, `docs/local-speech-input.md`.
- **Finding:** Watch repeatedly captured bounded microphone chunks, transcribed them automatically, and injected transcripts as chat turns. That made reception itself trigger interpretation and conversation.
- **Fix:** explicit Ears start now continuously receives raw 48 kHz stereo PCM into a 16-second in-memory rolling buffer. The capture worker does not transcribe or classify sound. A deliberate `ears listen` request records the next requested 1–15-second interval from the live buffer and passes it to the existing local Whisper bridge; if continuous capture is not active, the existing direct capture path remains available. Watch now coordinates raw audio capture and an already-open Eyes source without automatically transcribing or creating chat turns.
- **Brain-selected attention:** conversation and autonomous reflection can deliberately emit `[EARS_LISTEN] recent|seconds` to inspect already-buffered sound or `[EARS_LISTEN] live|seconds` to listen to the next interval. The actual result is returned to the brain for interpretation; the tool refuses to overwrite an already-open non-continuous audio source, and autonomous listening yields when foreground conversation takes priority.
- **Limit:** Qwen 3.5 9B does not accept raw audio, and the current Whisper bridge recognizes speech rather than classifying non-speech environmental sounds. The sensory input path is separated from automatic interpretation, but semantic sound recognition is not claimed as implemented.
- **Regression coverage:** `tests/ears_speech_bridge_smoke.py`, `tests/watch_coordinator_lifecycle_smoke.py`, and `tests/vision_watch_continuous_feed_smoke.py`. Live microphone and cancellation behavior still require target-host testing.

### R2-AUD-2026-10-28-011 — Database audit did not inspect foreign-key or cross-organ reference integrity

- **Area:** `tools/audit_r2_databases.py` and `tests/database_connection_audit_smoke.py`.
- **Finding:** the prior read-only audit reported SQLite `integrity_check`, table inventories, and selected legacy rows, but it did not report `foreign_key_check` results or dangling references between separate reward and memory databases.
- **Fix:** the audit now reports SQLite foreign-key violations and checks the known Life Log, diary, sensory journal, visual library, Alternate-Self, and reward-to-Life-Log references without writing to the source databases. Missing legacy tables/columns are reported as checks not run; actual orphaned references and FK violations are failures. Added a fixture test proving it detects an orphaned reward-to-Life-Log reference and an in-file foreign-key violation while preserving source rows.
- **Limit:** this is a read-only audit implementation and fixture regression. The user's actual `/home/x/R2_Home/R2` database files have not been accessed in this session; run the audit against that installation to determine whether its existing data needs repair. No live database was migrated or modified by this workload.


### R2-AUD-2026-10-28-012 — Legacy Alternate-Self branches could be disconnected from the Life Log

- **Area:** `AlternateSelf.c`, shared `r2_memory.db`, database relationship audit.
- **Finding:** the additive migration adds `log_event_id INTEGER NOT NULL DEFAULT 0` to older Alternate-Self branch tables. That makes the old table writable, but leaves pre-existing branches pointing at event ID zero, which is not a real Life Log event. The old hypothetical itself survives while its required event relationship does not.
- **Fix:** Alternate-Self initialization now finds branches whose Life Log link is missing, nonpositive, or dangling. It creates (or reuses, if a prior interrupted migration already created it) a dedicated Life Log migration event and updates the branch link before the organ reports initialized. It preserves the existing branch name, scenario, assumptions, prediction, conclusion, and status. If it cannot restore the required link, initialization fails rather than exposing an incorrectly linked branch.
- **Regression coverage:** `tests/alternate_self_smoke.c` now seeds an unlinked legacy branch and verifies that initialization restores its Life Log relationship. The read-only database audit reports orphaned relationships across the shared memory database and the reward ledger.
- **Scope:** this migration runs only when R2 initializes its database. The user's installed database has not been accessed here, so the audit output from that machine is still needed to identify any historical orphaned rows outside this known schema migration.


### R2-AUD-2026-10-28-013 — Legacy Life Log organ records could retain default-zero event references

- **Area:** `Log.c`, shared `r2_memory.db`, read-only database relationship audit.
- **Finding:** the additive migration adds event-link columns to older Life Log organ tables with `DEFAULT 0`. If those tables already contained rows, the new columns could remain pointed at nonexistent event ID zero. Older `r2_log_links` schemas could also retain their original `event_id` and `related_event_id` endpoints while the new `from_event_id` and `to_event_id` columns stayed zero.
- **Fix:** the migration now maps the old link endpoint names when they exist, then restores missing event links for legacy activity sessions, milestones, conversations, inner states, beliefs, continuity items, hypotheticals, and system snapshots. Each repair creates or reuses a Life Log migration event, preserves the original organ record, and updates its required event reference. Required relationships are validated before Life Log initialization succeeds; unmappable old log-link endpoints are not silently accepted.
- **Regression coverage:** `tests/life_log_legacy_schema_smoke.c` now seeds legacy versions of all eight linked record types plus old-style `r2_log_links` endpoints and verifies that their links resolve after initialization. `tools/audit_r2_databases.py` also reports foreign-key violations and dangling references across the Life Log, diary, sensory journal, visual library, Alternate-Self, and reward ledger.
- **Limit:** fixture tests exercise legacy schemas and migration logic. The user's actual databases remain uninspected until the read-only audit is run against `/home/x/R2_Home/R2`.


### R2-AUD-2026-10-28-014 — Camera snapshots and voluntary media attention were not fully integrated

- **Area:** `r2.c`, `r2.h`, primary Eyes camera, auxiliary media display, conversation Ollama payload, autonomous reflection.
- **Finding:** deliberate camera observation could independently consume frames from the FFmpeg camera pipe while the continuous Eyes worker owned that same stream. It could also fail before the worker's first published frame; direct conversation serialization could return a stale image after capture finished. The display received continuously captured frames, but R2 had no sustained voluntary choice to watch it across model turns.
- **Fix:** deliberate camera observation and active conversation now copy the same owned current-frame snapshot, waiting for the first published frame and rejecting the terminated worker's last stale snapshot. `[MEDIA_FOCUS] watch` and `[MEDIA_FOCUS] away` let the conversation and reflection model choose or release sustained display attention; that choice never starts playback, replaces camera Eyes, or triggers image interpretation from raw capture. When R2 has chosen to watch an available display and has a live camera image, the Ollama message contains the camera image first and the independent display frame second. The focus state is cleared if the selected display source stops or is replaced rather than silently following a different display.
- **Regression coverage:** `tests/camera_media_attention_contract_smoke.py`, `tests/vision_watch_continuous_feed_smoke.py`, `tests/media_focus_bridge_smoke.py`, and compiled `tests/media_focus_payload_runtime.c`.
- **Limit:** a current frame delivered to a model turn is not equivalent to consuming every frame of a video or possessing temporal awareness of a whole movie. Real Qwen model acceptance of a two-image turn on the installed 4 GiB GTX 1050 Ti remains a target-machine check, not proven by JSON/regression tests.

### R2-AUD-2026-10-28-015 — Sensory Journal could guess the wrong diary identity from duplicated prose

- **Area:** `SensoryJournal.c`, `SensoryJournal.h`, `r2_diary.c`, the shared `r2_memory.db`.
- **Finding:** the Journal's `diary_entry_written` / `reflection_completed` link projections matched diary rows by `d.entry=e.details` and selected the newest identical text. Two legitimate entries with identical prose could therefore be linked to the wrong diary row. Meanwhile newer diary entries already had an exact durable `r2_diary_entry_links(diary_entry_id,log_event_id)` relationship, but their `diary_entry_linked` Life Log events were not projected into the Journal by that authoritative ID relationship.
- **Fix:** new diary linkage is projected by the exact committed event ID and diary row ID. Journal startup reconciles preexisting authoritative diary links; active diary writes invoke `r2_sj_link_diary_event` once the diary's durable ID relationship is committed. Legacy text links remain supported only if the text has exactly one candidate row. Ambiguous legacy text matches are displayed without a fabricated diary ID, including when a previous version already wrote a guessed association. Existing diary text and underlying Life Log rows remain untouched.
- **Regression coverage:** existing `tests/sensory_journal_search_smoke.c`, isolated diary fixture, and compiled `tests/sensory_journal_diary_identity_runtime.c` with duplicated text, distinct IDs, live insertion, and restart reconstruction.
- **Limit:** installed databases were not read here; tests use disposable local fixture databases. Full source/organ/database inspection and actual host-data acceptance are still required.

### Deferred next workload, requested after completion of current sensory/database work

Manually inspect every line in every tracked source file, not merely signatures, build results, or TODO searches. For each declared or introduced subsystem, trace actual execution and data flow from initialization through runtime, persistence, recovery, failure handling, and shutdown; finish incomplete paths rather than calling presence of a scaffold an implementation. Follow the interfaces among brain, Eyes, Ears, diary, Reality, Life Log, rewards, other organs, and all five existing databases, including backward-compatible historical records. Then audit process and model allocations, fixed output/input/context budgets, temporary buffers, truncations, and resource headroom so data and instructions are not silently shortened, while respecting actual hardware limits. Do not add autonomous-behavior safeguards or replace model choices with deterministic substitutes without asking the project owner. Do this **after** the current workload; neither this note nor green CI claims that exhaustive audit has already happened.

### R2-AUD-2026-10-28-016 — Local Whisper transcription received an incompatible 48 kHz stereo WAV

- **Area:** `r2.c` continuous Ears microphone/desktop buffers, explicit local speech-recognition bridge, `docs/local-speech-input.md`.
- **Finding:** Ears correctly captures independent 48 kHz stereo 16-bit PCM, but `r2_ears_listen_cancelable` previously sent that 48 kHz stereo file directly to `whisper-cli`. whisper.cpp's sample WAV reader requires 16 kHz input. A valid RIFF header at the wrong rate cannot be relied on as a working local transcription bridge, even with a working microphone and installed Whisper model.
- **Fix:** preserve both raw continuous Ears streams and histories. After an explicit 1–15 second listening action captures its full interval to a private WAV, convert **only the temporary clip** to 16 kHz mono 16-bit PCM in bounded chunks, with stereo averaging and a 3:1 downsample. Verify the original sample rate/channels/data size before conversion; reject an incomplete file rather than passing corrupted audio to Whisper. Invoke the configured local `whisper-cli` with the compatible file; cleanup removes both temporary WAV files and the transcript. This does not automatically transcribe, analyze, or change input capture.
- **Regression:** `tests/ears_speech_bridge_smoke.py` asserts the converted file reaches the Whisper executable. Compiled `tests/ears_whisper_wav_runtime.c` checks sample rate, channels, bit depth, full clip duration, converted signal values, original capture preservation, and a truncated source.
- **Limit:** even with the correctly normalized WAV, successful actual speech recognition still depends on `R2_WHISPER_MODEL`, the executable, the real PulseAudio/PipeWire devices, and available CPU/memory on the target installation.


### R2-AUD-2026-10-28-017 — Independent media could retain old focus after its stream ended

- **Area:** `r2.c` independent auxiliary media FFmpeg worker and Qwen visual handoff; `tests/media_live_snapshot_runtime.c`.
- **Evidence:** `r2_media_capture_worker` set the stream's running flag to zero after spontaneous capture failure but left the model-chosen `media_attention_selected` flag set until a later explicit stop. A subsequent available display might inherit an old watch decision. Deliberate `[MEDIA_SEE]` and current-turn media snapshots also copied only if the first frame had already happened to arrive, otherwise immediately returning unavailable although capture was genuinely underway.
- **Fix:** worker completion now clears chosen media focus and logs source loss rather than an intentional decision to look away. Current display capture owns a frame-generation counter, a completion flag, and a condition variable. The worker signals after publishing complete frames and upon ending; closing/replacing a display wakes consumers before joining capture. Both explicit media observation and passive current-turn media imagery share the same ownership-preserving snapshot helper, wait for the initial actual frame, and reject a stopped worker's stale last frame. Camera remains completely independent; availability never forces R2 to interpret the display.
- **Regression:** `tests/media_focus_bridge_smoke.py` checks source lifecycle and snapshot routing. `tests/media_live_snapshot_runtime.c` simulates a delayed first frame, end-before-first-frame, stale buffer after EOF, and spontaneous capture failure that clears attention. No installed camera, VLC, or real local Qwen run was performed.

### R2-AUD-2026-10-28-018 — Desktop playback transcriptions were mislabeled as microphone speech

- **Area:** `r2.c` explicit Whisper listening results and sensory Life Log, independent microphone and desktop Ears sources.
- **Evidence:** the same Whisper bridge accepts `desktop_live` and `desktop_recent` as well as microphone modes, but its success event always said a microphone utterance was transcribed. That contradicted actual source provenance in R2's persistent experience history.
- **Fix:** the sensory event now states whether audio came from the microphone or desktop playback, its live/recent mode and requested duration, without persisting raw PCM or forcing automatic transcription. `tests/ears_dual_attention_wiring_smoke.py` asserts the source-specific event handling.


### R2-AUD-2026-10-29-019 — Ordinary Reward falsely recognized as a completed once-only reward

- **Area:** `Reward.c` idempotent reinforcement, `r2_rewards.db`, cross-organ Addiction replay.
- **Finding:** `r2_reward_apply_once(target,source,...)` found its purported prior event using `SELECT ... FROM reward_events WHERE target=? AND source=? LIMIT 1`. That query ignored the `reward_once_keys` ledger entirely. An earlier *ordinary*, non-idempotent reward with the same target/source could make a later genuine one-time reward return "already applied", attach to the wrong event, and misattribute any voluntary Addiction reinforcement.
- **Fix:** add a nullable `reward_event_id` column to the existing once-key table with an additive migration, without destroying old reward rows or keys. Under one `BEGIN IMMEDIATE` transaction, newly created one-time keys are bound to the actual inserted reward event ID before commit. A retry identifies its reward **only through that key's exact event ID** and verifies the target/source pair; arbitrary ordinary rows can no longer act as one-time keys. For pre-ID keys, a sole historical candidate or a uniquely creation-time-matched candidate can be linked; key-only legacy rows can complete a missing event. Multirow ambiguous old histories with no distinct timestamp return an explicit error rather than silently choosing an arbitrary row. Old unknown key creation times remain unknown rather than being overwritten with today's date.
- **Regressions:** `tests/reward_addiction_replay_runtime.c` now verifies ordinary-then-once behavior, retry, exact key ID, restart, single-row pre-ID migration, ambiguous multirow preservation and completion of a key-only old transaction. `tests/reward_legacy_schema_smoke.c` asserts new additive field without alteration of existing data; `tests/reward_atomic_idempotency_smoke.py` checks key/event/total write ordering.
- **Caveat:** original identity in genuinely ambiguous pre-ID once-keys cannot be reconstructed with certainty from their old schema; R2 preserves such rows for explicit review instead of fabricating history.

### R2-AUD-2026-10-29-020 — Oversized Sensory Journal output could falsely report empty history

- **Area:** `SensoryJournal.c` read-only recent/search serialization and `tests/sensory_journal_search_smoke.c`.
- **Finding:** an existing record exceeding the Journal's 2 MiB display ceiling set `intentionally_truncated` while `len == 0`, then returned "No Sensory Journal entries matched" (or "no entries yet"). Near the response-buffer boundary the truncation note could be omitted entirely. Neither outcome reflects persistent SQLite contents.
- **Fix:** never emit empty-history assertions on truncated results; always append an explicit incomplete-result notice, growing just the response buffer needed to show it. Preserve the existing bounded UI view and every original journal row rather than pretending the entire result was returned.
- **Regression:** compiled Journal search fixture inserts and indexes a real oversized sensory row and checks both search and recent output for an honest incomplete marker and absence of false empty-history claims.


### R2-AUD-2026-10-29-021 — Chosen independent display silently disappeared without a camera frame

- **Area:** `r2.c` Ollama image capture/serialization; `tests/media_focus_bridge_smoke.py`, `tests/media_focus_payload_runtime.c`.
- **Finding:** the active-turn payload gathered a chosen display frame only when `owned_image` (the primary camera snapshot) existed; the JSON helper also created `images` only when `image_base64` existed. A valid separate video source therefore became invisible to R2's conversation brain when camera capture had no current frame, contradicting the design of independent world sources.
- **Fix:** a deliberately selected, live media display is captured independently of camera success. The JSON serializer supports media-only image payloads; when both sources exist, the camera image remains first and display image second. Passive media capture does not force R2's focus or trigger an automatic recognition pass.
- **Verification:** extended actual JSON-payload fixture for camera-only, camera-plus-display, display-only and text-only turns; updated source wiring regression. Live media/Qwen hardware verification remains outstanding.

### R2-AUD-2026-10-29-022 — Chat was not using the same durable learning/recall contract as sensory history

- **Area:** `Log.c`, `SensoryJournal.c`, `tests/chat_sensory_shared_history_runtime.c`, `r2_memory.db`.
- **Finding:** `r2_log_conversation_turn` clamped both the user's and R2's original message length to a half-event preview maximum *before* inserting into the allegedly full-text `r2_log_conversations` archive. The remainder was silently lost. Both Life Log and Sensory Journal searched only bounded `r2_log_events` details, never the dedicated full chat archive, so text beyond the excerpt could not participate in later recall.
- **Fix:** store the complete original user and assistant strings using `sqlite3_bind_text64` in the existing conversation table, with no rewrite of older rows. Keep the chronological event as a clearly labeled, compact preview rather than claiming it is the full turn. Search the full archived user and assistant text from both Life Log and Sensory Journal using the exact conversation event ID. Life Log results provide a bounded excerpt around the deep-archive query match to supply meaningful evidence instead of only a first-paragraph preview. Do not modify the model's autonomous decision whether an experience becomes a higher-level preference or persistent memory.
- **Verification:** new compiled SQLite fixture inserts a real sensory observation and a long conversation containing distinct late user/assistant markers, checks their full stored lengths, both organs' indexed identities, deep-tail search, and recovery after restart.
- **Remaining scope:** these tests exercise shared storage/search and no forced learning. They do not verify a real Qwen session's attention, post-turn memory choice, or all brain/organ learning feedback; those require separate full system and host acceptance testing. The previously requested exhaustive file-by-file audit remains incomplete.

### R2-AUD-2026-10-30-023 — Memory evaluation allocation and disconnected reflection learning

- **Resume point:** `4845a3991132f2d77bab8ca72f59a8531fd3ba1c`. New branch `integrate/r2-shared-learning-2026-10-30` incorporates current `main` ancestry and already includes the priority-fixes branch. Three conflicting test expectations were compared to actual production behavior; the current bounded history implementation remains covered and the newer retry/retrieval assertions are retained. No update to `main` is part of this work.
- **Findings:** `memory_decision` estimated label/instruction overhead as 64 bytes, which was smaller than the actual formatted text and truncated the final evaluation instruction. `build_learning_context` clipped each reward/repeated-interest summary to 900 characters. Accepted autonomous and requested diary reflections were persisted, but never passed through chat's persistent-memory decision path. The autonomous sensory follow-up's actual tool evidence was also freed before learning could use it.
- **Changes:** allocate the complete labeled evaluation text and learning summaries using `asprintf`. Share the existing model-evaluation/persistence path between chat and accepted reflections. Retain actual sensory follow-up evidence through reflection evaluation, distinguish reflection interpretations from recorded observations, and link a saved-memory event to its source event with `supports_memory`. The model still chooses whether and what to remember, including `NONE`; no deterministic preference or reinforcement is assigned by this bridge. Existing databases receive ordinary memory/event/link rows without a new schema or destructive migration.
- **Verification:** compiled allocation check with a 100,000-byte original verifies both source formats and their complete final instruction. New CI runtime fixture uses production HTTP request/response handling and SQLite persistence with controlled model replies for chat, reflection and `NONE`; verifies source links and restart. Controlled responses test wiring, not the quality of a live Qwen decision. Complete production compilation and repository regression results are tracked by the branch workflow.
- **Remaining:** live host/camera/audio/Qwen acceptance; outstanding organ implementation; exhaustive manual source/allocation audit. Existing global inference-window policies and other fixed budgets are not claimed removed by this focused allocation repair.

### R2-AUD-2026-10-30-024 — Correctly formatted model memory choices were silently dropped

- **Runtime finding:** the new production HTTP/SQLite test received `preference | I prefer quiet music.` successfully, but no preference row was persisted. `memory_decision` temporarily split on `|`, then `trim(category)` inserted a NUL in the whitespace before the separator. Restoring `|` left that earlier NUL intact; the caller saw only the category and could no longer find a separator. This also affected the preexisting chat path.
- **Fix:** serialize the parsed category and memory into a newly allocated complete decision string before releasing the original response. Preserve the original memory text after normal outer-whitespace parsing; no shorter buffers or alternate learning behavior.
- **Regression:** keep an ordinary spaced `preference | ...` model response and a leading/trailing whitespace `knowledge` response in the production HTTP test, then assert exact memory rows, source links, `NONE` handling, and restart.

### R2-AUD-2026-10-30-025 — Sensory evidence did not survive through recall and learning

- **Manual path inspection:** followed microphone/desktop capture → deliberate Whisper → tool result → final chat → post-turn memory evaluation, and visual-library/Sensory Journal storage → retrieval. Ears logged only that transcription happened, discarded the original transcript file, and rejected output over 16 KiB. Chat learning received user/reply text without actual tool observations. Visual recall silently clipped every record to 8191 bytes. Sensory Journal capped result allocation at 2 MiB, shortened search text to 1024 bytes, and returned chat previews even for matches found only in the complete archive.
- **Implementation:** read completed transcript files using their actual lengths; persist recognized text with microphone/desktop and live/recent provenance in existing sensory Life Log rows. Keep raw capture and deliberate attention behavior unchanged. Persist complete chosen tool outcomes as conversation events, link them to their exact chat turn, and load those original details into the existing memory evaluator. The user message and final reply remain independently archived verbatim. Add an exact-event-details read API. Allocate complete Visual and Journal result rows; Journal returns full archived chat text and uses complete literal substring queries, including long queries sharing a 1024-byte prefix. The event writer binds full text with SQLite's 64-bit interface rather than refusing summaries/details at the previous fixed sizes.
- **Concurrency:** camera and microphone provenance used to link a chat turn were read while capture/source changes could mutate the objects. Copy both events under their source's capture mutex, then release it before database logging, matching the independent desktop path.
- **Regression scope:** extend compiled fixtures with a 72,000-byte transcript, exact event readback and Journal recall, actual tool evidence in the production HTTP memory request, full >2 MiB Journal rows, distinct long search strings, full deep chat-tail result text, and a >24 KiB visual record with a UTF-8 tail. No automatic transcription or semantic interpretation was added to sensor workers.

### R2-AUD-2026-10-30-026 — Chat archive writes were not atomic with their chronological event

- **Finding:** a conversation event was committed and Journal-indexed before the complete archive insert. If archive insertion failed, the function still returned a positive event ID; callers could not distinguish an excerpt-only partial write from a fully preserved chat turn.
- **Fix:** share the locked event insert implementation with a `BEGIN IMMEDIATE` transaction covering both original conversation strings and the chronological event. Commit before publishing the Journal projection. An archive failure rolls back the event and returns failure; it does not claim success from an excerpt. Existing history/schema is preserved.
- **Regression:** a SQLite trigger deliberately aborts archive insertion; assert failure, no orphan event, and no Journal projection. Existing long-conversation and restart tests then exercise normal successful persistence.
- **Remaining:** live host/device/Qwen acceptance, other unfinished organ functionality, and the exhaustive all-source/allocation audit are still separate work. This pass fixes the storage/read/learning evidence paths above; it does not claim all global inference-window and reflection-context budgets have been removed.

### R2-AUD-2026-10-30-027 — Common tool formatter clipped observations before handoff

- **Finding:** the shared `process_tools_with_status` APPEND macro used an 8192-byte stack formatting buffer and appended a truncation note. Even complete Journal/Visual results therefore lost their tail before entering live conversation and durable tool-evidence storage.
- **Fix:** allocate the full formatted text with `asprintf`, then grow the aggregate output to fit every byte. Preserve the existing allocation-failure reporting and capacity ownership; no reduced representation or fixed replacement ceiling.
- **Regression:** send the actual 72,000-byte persisted transcript through the real `[SENSORY_JOURNAL] search` tool, assert the complete result, persist that result, and recover it intact as post-turn learning evidence. Separate HTTP/SQLite checks verify model-selected learning with real tool evidence; this does not claim the existing global model-context limits have been audited away.

### R2-AUD-2026-10-30-028 — Complete sensory evidence was still clipped in model requests

- **Manual inspection:** after fixing storage and tool formatting, followed the same evidence into `ollama_chat_with_limit`. A 12,000-character per-message ceiling still shortened helper inputs without an authoritative human-query binding. Even a correctly archived/returned 72,000-byte observation therefore could not reach the learning model intact. The newest-sixteen-message limit also omitted fitting conversation history. The separate direct terminal entry point still used the obsolete 12,000-byte input capacity.
- **Implementation:** size each request's `num_ctx` allocation from its complete active input, the current turn's internal tool results, system override, generation allocation and protocol/context overhead. Grow in 1024-token increments using the existing serializer's character/token estimate. Keep the configured/adaptive host baseline as the starting allocation; do not shrink active evidence to that baseline. Remove the 12,000-character message, sixteen-message count and fixed 48,000-character aggregate inference ceilings. Reserve active text and this turn's tool outcomes before dividing the remaining system/history allocation. Select older complete turns rather than prefixes. Align the direct terminal's input allocation with shell/public chat capacity.
- **Regression:** the actual production HTTP memory request now carries the complete 72,000-byte transcript-derived tool evidence, and the controlled model endpoint checks the tail plus an increased `options.num_ctx`. Verify all 21 fitting history messages reach the endpoint; exercise growth for a large standalone helper input, current tool evidence, and retention of a larger starting baseline. Existing prompt-share, UTF-8, metadata construction, direct-input and active-turn identity checks continue to cover their unchanged responsibilities.
- **Limits:** context sizing uses the existing character/token estimate, not exact tokenization. Larger requests still require the model/server and installed RAM/VRAM to support the allocation; no real host Qwen/GPU acceptance was performed. System-section shares, output/response policies, other retrieval/reflection budgets, remaining organ functionality and the exhaustive all-source audit are not claimed complete by this pass.

### R2-AUD-2026-10-30-029 — A single attached image had ambiguous source identity

- **Finding:** the payload correctly supported a deliberately selected media display without a primary Eyes frame, but its one-element `images` array carried no source map. The brain could not identify from the payload whether that first/only frame was primary Eyes or the separate display.
- **Fix:** add a checked system-role source map immediately before the actual current image-bearing message, identifying primary Eyes, separate display, or both in their actual attachment order. Text-only requests receive no source map; original user content and image bytes remain intact. Metadata identifies sources without interpreting pixels or changing attention selection.
- **Regression:** extend the compiled JSON fixture for primary-only, display-only, dual-source and text-only source mappings; preserve existing original-text and exact image-order checks.

### R2-AUD-2026-10-30-030 — Fixed Ears clip size prevented complete chosen live listening

- **Manual source trace:** the brain and shell both limited listening to 1–15 seconds. The live bridge allocated the entire chosen clip, then waited until all samples fit simultaneously in a fixed 16-second ring; longer intervals could never complete. The same conversion function also rejected longer PCM files. In addition, a stop/restart reset byte counters without giving waiting consumers an identity for their original source, allowing replacement audio to be mistaken for the requested interval. Whisper was killed after an unrelated fixed 90-second wall-clock deadline, even if a valid requested transcription was still running.
- **Implementation:** consume each complete chosen live interval incrementally from the selected source's continuous ring into the private WAV. Preserve every PCM byte and sample rate; wrap-aware copying keeps memory proportional to a processing chunk rather than shortening or requiring the entire clip to occupy rolling history at once. Microphone and desktop have separate generation counters and condition notifications. Capture the original generation with its start position; a replacement source cannot fulfill an old interval. Publication, completion and lifecycle changes notify readers, with cancellation checks while waiting. Recent mode continues to require actual retained historical samples and cannot invent audio that was never retained.
- **Duration/ASR:** replace the 15-second policy with the real RIFF/WAV 32-bit length representability bound shared by brain, direct API and shell. Normalize the full completed clip for Whisper regardless of the former duration. Remove the fixed ASR completion deadline; explicit cancellation, foreground priority and shutdown still terminate/reap the child normally. Update help and speech documentation.
- **Runtime regression:** a synthetic producer and the production live-reader deliver all 30 seconds of PCM for both microphone and desktop, crossing the ring boundary and wrapping, while checking byte-exact content and the untouched other source. Test source replacement with reused counters, overwritten source intervals, cancellation and ended capture. The WAV fixture checks full 30-second normalized duration and RIFF arithmetic. Fixtures exercise production C paths without PulseAudio devices or a real Whisper model.
- **Remaining:** real device/model acceptance, blocking Pulse stream lifecycle inspection, other unfinished organ paths, and the exhaustive all-source/allocation audit remain outstanding. This change is not a claim that arbitrary past audio beyond retained history can be recovered.
