# moonlight-common-c (shared copy)

This directory is the single copy of the Moonlight protocol library used by the NyxPlay
Host, the NyxPlay PC client and the NyxPlay Android client. No other copy of the library
may exist in the repository (see `common/README.md` and `common/check-single-copy.ps1`).

| Field | Value |
|-------|-------|
| Upstream | https://github.com/moonlight-stream/moonlight-common-c |
| Upstream commit | `94c65fd` (2026-11-13, "Remove references to the deprecated Chrome client") |
| Apollo extensions | https://github.com/ClassicOldSong/moonlight-common-c, commits `84af637` and `c999436` |
| ENet (submodule `enet`) | https://github.com/cgutman/enet, commit `98dbb89` (`moonlight` branch used by upstream) |
| nanors (submodule `nanors`) | https://github.com/sleepybishop/nanors, commit `a9b1636` |
| License | GPL-3.0 with the App Store distribution exception, see `LICENSE.txt` |

The files are imported as plain files, not as a subtree. The upstream history is not
part of this repository; every file keeps its upstream licence header and the authors are
credited here and in the repository `CREDITS.md`.

## Authors

- Cameron Gutman, Diego Waxemberg, Michelle Bergeron, Iwan Timmer, Sunguk Lee and the
  other contributors of the Moonlight Game Streaming Project (upstream library).
- Yukino Song (ClassicOldSong): Apollo extensions `84af637` (server command, clipboard and
  file transfer nonce packet types, control channel 0x08) and `c999436`
  (`LiSendEmptyPayload`).

## Local changes (marked with `NyxPlay` in the code)

- Text input state extension: packet type `0x4E01`, `ConnListenerSetTextInputState`
  callback, asynchronous queue collapse and the `FakeCallbacks` default.
- Shared extension constants in `src/NyxExtensions.h`, used by the library and the host.
- Control stream packet type table layout: indices 12 to 14 are the Apollo entries, 15 is
  the adaptive triggers entry, 16 is the text input entry; `IDX_COUNT` and a compile time
  check keep every `packetTypesGen*` table the same length.
- RTSP resilience: idle and total timeouts, polling with `ConnectionInterrupted`, bounded
  retry of OPTIONS/DESCRIBE/SETUP after a connection reset before any byte arrived,
  plaintext response completeness check after a reset, and `freeMessage` leak fixes.
- `src/lc_atomic.h`: C++ safe replacement for `<stdatomic.h>` so that the headers can be
  included from the host (C++20), also from inside an `extern "C"` block.
- Apollo clipboard and file transfer nonce messages received from a host are dropped quietly
  (they are flagged asynchronous but no client handles them; debug builds asserted).
- Upstream `.github/` is not carried over.

## Differences that are intentional

- The NyxPlay Host keeps its own nanors (`host/third-party/nanors`, `19f07b5`) for its
  FEC encoder. The library test suite proves that both versions produce identical shards.
- ENet is the unmodified upstream submodule; the host keeps `control_intercept.cpp` for
  its peer address handling.

## Updating

Compare against upstream with `git diff --no-index` on a fresh checkout of the recorded
commit, merge the upstream changes by hand, keep the local changes above, then update the
commit in this file and run `common/tests/run-tests.sh`.
