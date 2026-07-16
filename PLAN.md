# Starfish sync & threading overhaul — plan

Status of a multi-phase fix program for the webOS Starfish port, based on a
full audit of the fork's diff vs upstream (merge base `02273e9d9e`). The code
work is **complete**: Phase 2 is `d3c0fab1ad`, Phase 3 is `c62ec40d82`, and the
Phase 4 cleanup is `cf18e4cb67`. Automated on-TV checks are also complete.
The subjective lip-sync/seek checks and the product decision about removing
`ao_starfish` are intentionally recorded in `../UNFINISHED.md`.

Background for every phase: the audit found that most historical sync
symptoms were fought with compensating mechanisms (grace windows, watchdogs,
settle timers) layered on top of measurements that were wrong at the source.
The program fixes the measurements and then *removes* layers.

---

## Phase 0 — DONE (committed)

| Change | Where | Why |
|---|---|---|
| AO delay float division | `audio/out/ao_starfish.c` `get_state()` | `queued_samples / samplerate` was int/int → `state->delay` was always `0.0`, so `ao_get_delay()`/`playing_audio_pts()` overstated the audio position by the whole AO queue (0.24–0.58 s). |
| Single-flight audio feed + flush generation | `audio/out/ao_starfish.c` `feed_pending_packets()`, `free_pending_packets_locked()` | The drain loop ran concurrently from the AO thread and the ctx worker (via `audio_prime_cb`), reordering audio ES packets exactly at seek/segment boundaries; `FEED_AGAIN` re-queues could resurrect a pre-flush packet. `p->feeding` guard + `p->pending_generation`. |
| Clock anchored to frame flips | `video/out/starfish/starfish_ctx.cpp` `accept_clock_sample_locked()` (new `quantized_sample` param, new `clock_last_poll_host_ns` field) | `getCurrentPlaytime()` is frame-quantized. The host anchor used to advance on *every* poll even when the pts hadn't changed, so `pts + age` projections reset to the frame-start pts each poll — video clock read low by up to a frame (mean ~half frame), audio slaved to it ran early by the same amount. Now the anchor stays at the poll that first saw the pts; on advance, midpoint bracketing. |
| Callback teardown fence | `starfish_ctx.cpp` `call_wakeup()` / `call_audio_prime()` | Copy-then-invoke had a UAF window vs AO/VD teardown. Client lock now held across invocation; `set_*(nullptr)` is a synchronization point. Lock order verified safe: filter destructor doesn't hold `async_lock` during `destroy()`; `ao_uninit` joins the AO thread and holds no buffer locks during `driver->uninit`. |
| Controller retune | `player/audio.c` constants + deadband branch | Deadband 50 ms → 10 ms (a 24 fps frame = 41.7 ms sat inside it forever), EMA 5 s → 1 s, recovery 12 s → 4 s, hard realign 350 ms → 150 ms; stopped zeroing the filtered error inside the deadband (threw away evidence, caused threshold dithering). |
| Resume held video on reset | `player/playloop.c` `reset_playback_state()` | Seek while `starfish_video_held_for_audio` cleared the flag without sending `VOCTRL_RESUME` → pipeline stayed paused (raw VOCTRL bypasses `vo_set_paused` bookkeeping), no frame ever produced, playback wedged until manual pause/unpause. Now calls `release_starfish_video_for_audio_clock()`. |
| Phase-1 test hooks | `starfish_ctx.cpp` | (a) `STARFISH_AUDIO_FEED_AHEAD_MS` env: caps how far ahead of the sampled clock audio ES is fed (only once `started && clock_sample_valid`, so preroll can't starve). Unset = old behavior (shared 1.6 s cap). (b) 1/s "periodic" worker status log while PLAYING (reuses `maybe_log_worker_status_locked`). |

Verification done locally: full host build of `mpv_webos` compiles
(`nix develop -c meson setup <dir> mpv_webos -Dlibmpv=true -Dcplayer=false`
then ninja); `ao_starfish.c` and `starfish_ctx.cpp` pass
`-fsyntax-only` with host ffmpeg/libplacebo headers (starfish sources don't
build on Linux — no LG SDK). **Nothing has run on the TV yet.**

---

## Phase 1 — AUTOMATED ON-TV CHECKS COMPLETE; SUBJECTIVE CHECKS DEFERRED

You are guiding the user through validating Phase 0 and running the
ao_starfish lag experiment. Read `../AGENTS.md` first: TV is
`root@192.168.0.200`, app log is `/tmp/com.codex.jellyfinnative-mpv.log`,
use `ssh -tt` for `luna-send`, read-only over SSH, deploy only via
`ares-install`/supported tools, and don't start expensive webOS build loops
without telling the user (they are expected here — this phase is testing).
Build/deploy of the webOS mpv is via the repo's existing workflow
(`tools/`, GH-actions-mirroring scripts); ask the user which script they
normally use for the mpv `.ipk`/libmpv rebuild rather than guessing.

### 1A. Regression + sync check for VO starfish + AO alsa (the working config)

1. Build & deploy with Phase 0 included. Run with `--msg-level=all=v` (or
   whatever the app uses to enable verbose mpv logs).
2. Start playback of 24 fps content (worst case for the old deadband).
   Watch for `Starfish ALSA sync audio=... filtered=... speed=...` lines:
   - **Pass:** `filtered` converges to within ±0.010 within ~15 s of start
     and stays there; `speed` hovers within 1.0 ± 0.005; no audible pitch
     artifacts.
   - **Fail modes to watch:** `speed` oscillating between corrections and
     1.0 once per few seconds → deadband still too tight for residual clock
     noise; raise `STARFISH_AUDIO_SYNC_DEADBAND` to 0.015 before touching
     anything else. Frequent `hard realign` warnings (>1 per minute in
     steady state) → raise `STARFISH_AUDIO_SYNC_HARD_REALIGN_THRESHOLD`
     back toward 0.250; the 0.150 value assumes the anchor fix made the
     measurement clean.
3. Subjective lipsync check (talking-head content, 24 fps and 60 fps). The
   old code sat up to ~40 ms audio-early; the user should specifically judge
   whether the "maybe a frame off" feeling is gone.
4. Seek torture: 20+ seeks including rapid scrubbing (seek during restart —
   this used to be able to wedge playback via the video-hold bug). Confirm
   playback always resumes, audio rejoins within ~1–2 s, no permanent
   silence, no frozen video needing pause/unpause.
5. Pause/unpause during the first 2 s after a seek (hold window) — same
   checks.
6. Runtime `audio-delay` changes up and down — still latches and converges.

### 1B. ao_starfish lag experiment (the "1–2 s off" investigation)

Hypothesis from the audit: the SDK's ES audio sink paces loosely
(FIFO-ish) rather than scheduling strictly by PTS, so audible lag ≈
SDK-side feed lead (up to the 1.6 s `MAX_FEED_AHEAD_NS`). The AO never
measures the pipeline position, so nothing could correct it.

1. Switch the app to `--ao=starfish` PCM mode (`STARFISH_AUDIO_CODEC=pcm`).
2. Baseline run, no cap. Collect the 1/s worker heartbeat lines
   (`Starfish worker status reason=periodic ... fed_v=... fed_a=...
   clock=...`). Compute steady-state `fed_a − clock`:
   - If it sits ~1.5 s AND audible lag ≈ that number → hypothesis
     confirmed.
   - Also note `fed_v − clock` (video lead is expected and fine).
3. Re-run with `STARFISH_AUDIO_FEED_AHEAD_MS=300`, then `150`. Expected if
   confirmed: audible lag shrinks proportionally. Watch for underruns
   (`BUFFERLOW`, stuttering audio) — if 150 stutters but 300 is fine, note
   the floor.
4. Outcomes:
   - **Confirmed:** proceed to Phase 3 (make the AO honest); the cap can
     ship as an interim default (set the default in `audio_feed_ahead_ns()`
     rather than requiring the env var) with a value the tests supported.
   - **Not confirmed** (lag persists with a 150 ms lead): the offset is a
     timebase/epoch problem instead. Next probes: compare the first audio
     packet pts fed after a segment prime vs the segment target in the log;
     check whether the AAC path (no `STARFISH_AUDIO_CODEC`) shows a
     *different* constant offset (AAC adds ~21 ms encoder priming delay —
     a different signature); dump `getCurrentPlaytime` right after PLAYING
     with audio-only content if the app supports it.
5. Whatever the outcome, save representative log excerpts into
   `notes/` or the PR description — Phase 3 design depends on them.

---

## Phase 2 — DONE (`d3c0fab1ad`)

1. **Route unload through the worker.** `starfish_ctx_unload()` calls
   `sf_backend_unload()` from client threads (vd destroy → playloop) while
   the worker may be inside `sf_backend_feed`/`Play`/`getCurrentPlaytime`.
   The SDK is not known thread-safe; every other SDK call is worker-only.
   Add `unload_requested`, have `worker_loop` perform the Unload (mirroring
   `apply_flush_locked`), and have `starfish_ctx_unload` wait on the cv for
   IDLE with the existing 2 s deadline.
2. **Fence `backend_event_cb` against destruction.** `sf_backend_destroy`
   should null `b->callbacks` under a dedicated callback mutex *before*
   calling `Unload`, and the trampoline should take that mutex — otherwise a
   late SDK event can race `delete ctx` (destroying a locked mutex is UB).
3. **Replace the pending-segment boolean spaghetti** in `starfish_ctx`
   (`pending_segment_ready_frame/_pts/_have_pts/_saw_play/_await_play` +
   watchdog time point, mutated from three threads) with one explicit
   `enum class segment_state { NONE, AWAIT_FRAME, AWAIT_PLAY, READY }` plus
   pts + deadline. Keep behavior identical first (mechanical translation),
   then simplify. Most historical seek races live in illegal combinations of
   those booleans.

## Phase 3 — DONE (`c62ec40d82`)

Replace the fictional device model in `ao_starfish.c` `get_state()`:

- `delay = (last_written_pts_ns − getCurrentPlaytime()) / 1e9`, using the
  ctx's cached clock sample (add a small accessor; do NOT call the SDK from
  the AO thread — the worker already samples every 20 ms).
- `playing` from actual pipeline state (PLAYING && fed), not the local
  `p->playing/paused` booleans plus grace windows.
- `free_samples` derived from a fixed feed-ahead target (e.g. keep 0.3–0.5 s
  ahead of playtime), which supersedes `STARFISH_AUDIO_FEED_AHEAD_MS`.
- Then DELETE: `drain()`, `buffered_samples`, `underrun_grace_until_ns`,
  `audio_delay_grace_until_ns` and the grace logic in `get_state` — they
  exist to paper over the model being unanchored.
- Alternative worth putting to the user before doing this work: if
  compressed passthrough (AC3/EAC3 via `configure_audio_passthrough`,
  currently unused) is not on the roadmap and PCM-over-ALSA is good, delete
  `ao_starfish.c` entirely (~1,250 lines) and standardize on VO starfish +
  AO alsa.

## Phase 4 — DONE (`cf18e4cb67`)

- **Dead code:** `mpctx->starfish_audio_start_bias` (read in 4 places, only
  ever assigned 0 — either wire it as a learned start-bias or delete),
  `starfish_ctx_get_current_pts` (exported, never called),
  `starfish_json_build_feed/seek/play_rate` (backend snprintf's its own
  payloads), `starfish_audio_clock_wait_start_ns` (only gates a log),
  `encode_pending_audio(…, flush_tail=true)` (never used).
- **Delete the AAC audio path** once PCM is confirmed (self-labeled
  "removable"; carries its own bug class — encoder priming delay, encoder
  reopen on every reset).
- **Dedupe** `is_starfish_video_out` / `is_alsa_audio_out` /
  `starfish_split_clock` / `query_external_video_clock` — copy-pasted into
  player/audio.c, player/playloop.c, player/video.c. One helper header.
- **Env vars → mpv options** (`STARFISH_AUDIO_CODEC`, `STARFISH_DOVI_POLICY`,
  `STARFISH_AUDIO_HINT`, `STARFISH_USE_EXPORTED_WINDOW`,
  `STARFISH_WINDOW_*`, `WEBOS_ALSA_NO_HW_PAUSE`, `WEBOS_ALSA_BOUNDED_IO`,
  `STARFISH_AUDIO_FEED_AHEAD_MS`): make them `--vo-starfish-...` /
  `--ao-starfish-...` / ALSA sub-options so they're documented, loggable and
  per-profile.
- **Upstream-merge isolation:** ao_alsa's
  `state->playing = … && queued_samples > 0` change applies to desktop
  Linux builds too (AppImage ships this driver) — gate it on `bounded_io`
  or webOS; `wayland_common.c`'s `wl_display_create_queue_with_name` →
  `wl_display_create_queue` swap and the meson wayland version floors are
  global — guard behind HAVE_STARFISH/compat macros. `wl_compositor` bind
  was lowered to ver≥1 while `vo_starfish` calls `wl_surface_damage_buffer`
  (a since-v4 request) — version-gate the call or re-raise the bind floor.
- **Efficiency:** `process_hdr10plus_packet` byte-scans whole HEVC packets
  for SEI on every feed attempt *including FEED_AGAIN retries* whenever
  `dv_profile == 0` — cache the transform result on the pending packet.
  Video packets are copied 3–4× on the way to the SDK — consider passing
  refcounted buffers into the ctx queue.
- **Bounded-IO ALSA delay:** in bounded mode the code discards ALSA's
  `delay` (drops hw/DAC latency) and period-aligns `free_samples` down
  (biases queued up by ≤1 period ≈ 21 ms) — evaluate trusting `delay` when
  sane, falling back only when it's the garbage value being defended
  against. Log both for a session to characterize the webOS driver first.
- Demote per-event `MP_INFO` chatter (SDK begin/end, per-segment blocks) to
  verbose/trace now that bring-up is over.
- `playback_speed != 1` is unsupported in split-clock mode
  (`SetPlayRate` is hard-coded to 1000): either wire it or clamp/refuse and
  document.
