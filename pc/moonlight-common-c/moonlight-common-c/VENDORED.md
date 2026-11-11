# Vendored: moonlight-common-c

| Field    | Value |
|----------|-------|
| Upstream | https://github.com/moonlight-stream/moonlight-common-c |
| Commit   | `04053843c90c4268583f2496b997d54d59f03957` |
| License  | GPL-3.0 (see `LICENSE.txt`) |

This copy is vendored into the NyxPlay monorepo so that NyxPlay specific
protocol extensions (for example the host driven text input state message)
can be maintained next to the PC client. Nested dependencies (`enet`,
`nanors`) remain git submodules and are declared in the repository root
`.gitmodules`.

All NyxPlay changes are marked with `NyxPlay:` comments.
