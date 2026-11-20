# Shared code

## moonlight-common-c

`common/moonlight-common-c` is the only copy of the Moonlight protocol library. The NyxPlay
Host (CMake), the PC client (qmake) and the Android client (ndk-build) all compile the files in
this directory. The upstream commit, the Apollo extensions, the NyxPlay changes and the
submodule pins are listed in `moonlight-common-c/VENDORED.md`.

Rules:

- Change the library here, never in a copy. Mark NyxPlay changes with a `NyxPlay` comment and
  list them in `VENDORED.md`.
- ENet and nanors are submodules of `common/moonlight-common-c`. The host keeps its own nanors
  (`host/third-party/nanors`) for its FEC encoder; `tests/` proves that both produce the same
  shards.

### One-copy guard

```
powershell -NoProfile -File common/check-single-copy.ps1           # transitional: old copies only warn
powershell -NoProfile -File common/check-single-copy.ps1 -Strict   # no other copy may exist
```

The guard fails when a second `Limelight.h` is tracked, when `.gitmodules` registers a
library checkout outside `common/`, when the shared copy lacks required files or carries a
`.github` directory, or when the ENet or nanors submodule differs from `VENDORED.md`. The
transitional mode tolerates the three old locations until they are removed; switch the
guard to `-Strict` in the same change that removes them.

### Tests

`tests/run-tests.sh` builds the library with `-Wall -Wextra -Werror` and runs the test programs
(security fixes, packet tables, replay windows, encrypted message vectors, FEC round trip
between both nanors versions, C++ header gate). Use `--san` for AddressSanitizer and
UndefinedBehaviorSanitizer (Linux). `tests/msvc-check.cmd` compiles the library with MSVC
the way the PC client does.

```
# MSYS2 UCRT64 shell
common/tests/run-tests.sh
```
