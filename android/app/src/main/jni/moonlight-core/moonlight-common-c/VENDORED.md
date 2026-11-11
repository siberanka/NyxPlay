# Vendored: moonlight-common-c (Artemis fork)

| Field    | Value |
|----------|-------|
| Upstream | https://github.com/ClassicOldSong/moonlight-common-c |
| Commit   | `c999436858471dfefa7617af3b7dc03ec1644ce4` |
| License  | GPL-3.0 (see `LICENSE.txt`) |

This copy is vendored into the NyxPlay monorepo so that NyxPlay specific
protocol extensions (for example the host driven text input state message)
can be maintained next to the Android client. Nested dependencies (`enet`)
remain git submodules and are declared in the repository root `.gitmodules`.

All NyxPlay changes are marked with `NyxPlay:` comments.
