# Realtime mixer scheduling follow-up (2026-10-23)

Baseline: the independent loader/decode/mixer implementation documented in
`AUDIO_REALTIME_20261023.md`, immediately before this follow-up. The original
frame-fed mixer is retained only as the starvation test's negative control.

## Changes

- Replace the fixed 2 ms mixer timeout with queue-level scheduling. Playing
  streams leave one and a half device periods of scheduling margin; empty and
  explicitly paused mixers wait for events. An SDL-only device pause retains a
  50 ms fallback because device state changes do not always send app events.
- Use a mutex-protected notification serial so load/decode readiness can satisfy
  the condition-variable predicate, including events arriving just before wait.
  Shutdown also pairs notification with the loader/decode wait mutexes.
- Predecode enough PCM to fill the existing target before starting a channel.
  EOF allows shorter sounds to start immediately. The first experimental build
  exposed a 1,024-frame source shortage when an immediate notification let the
  mixer overtake the first decode block; startup prefill resolves that case.
- Freeze the whole suspended interval, including the final interval on resume.
  A newer snapshot received while paused uses its own time anchor.
- Expose `waits` in `OA_AUDIO_DIAG` for before/after scheduling investigations.

Device queue targets, callback code, MIDI settings, and the movie audio path
retain the previous implementation.

## WSL validation

Ubuntu 22.04, SDL2 dummy audio, 44,100 Hz stereo, 1,024-frame device period:

| Scenario | Previous mixer wait entries | Updated wait entries |
| --- | ---: | ---: |
| 500 ms playing without main-thread updates | 244 | 43 |
| 100 ms explicit pause, after settling | 49 | 0 |
| 60 ms idle, after settling | 29 | 0 |

Wait entries are scheduling-loop counts, rather than a measurement of total CPU
utilization. During the 500 ms main-thread stall the updated mixer produced
22,528 frames. Both source shortages and device underruns remained zero through
that arm and a separate 350 ms blocked voice load. The old frame-fed negative
control accumulated eight device short reads.

The pause test records actual float PCM and compares its gain with independently
decoded source samples. A 400 ms pause retained the fade envelope (approximately
0.119 before / 0.122 after); a snapshot received during another pause resumed at
approximately 0.182. Resuming requires no subsequent gameplay frame submission.
An idle restart also checks that one submission is sufficient to wake all stages.

All 17 selected audio, media, interpreter, save, emote, and real-game-script
regressions passed. `audio_window` compared 811,440 samples with maximum error 0.
AddressSanitizer with leak detection passed the expanded realtime test. TSAN
remains unavailable in this WSL environment (the prior empty-process probe fails).

Evidence is under `workspace_temp/build_cache/engine-followup-20261023/` in the
WSL workspace: `artemis-before.log`, `regression.log`, and
`sanitizer-test-address.log`. These checks cover host behavior; Harmony device
audio timing still requires a subsequent device test.
