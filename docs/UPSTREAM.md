# Keeping NyxPlay in sync with upstream

NyxPlay follows three upstream projects. Their histories are connected to this repository so that
upstream fixes can be merged while NyxPlay specific changes are preserved.

| Directory | Upstream remote | Branch | How it is connected |
|-----------|-----------------|--------|---------------------|
| `android/` | `https://github.com/ClassicOldSong/moonlight-android` | `moonlight-noir` | Full history (this repository is a fork of it); moved into `android/` |
| `host/` | `https://github.com/ClassicOldSong/Apollo` | `master` | `git subtree` (squashed) |
| `pc/` | `https://github.com/moonlight-stream/moonlight-qt` | `master` | `git subtree` (squashed) |

## One-time setup

The canonical repository is `https://gitlab.com/siberanka/NyxPlay` (`origin`); `https://github.com/siberanka/NyxPlay`
is a mirror that keeps the GitHub fork network of Artemis intact.

```bash
git remote add artemis      https://github.com/ClassicOldSong/moonlight-android.git
git remote add apollo       https://github.com/ClassicOldSong/Apollo.git
git remote add moonlight-qt https://github.com/moonlight-stream/moonlight-qt.git
# Never push to upstream remotes by accident
for r in artemis apollo moonlight-qt; do git remote set-url --push "$r" no_push; done
```

## Pulling upstream changes

```bash
# NyxPlay Host <- Apollo
git subtree pull --prefix=host apollo master --squash

# NyxPlay for PC <- moonlight-qt
git subtree pull --prefix=pc moonlight-qt master --squash

# NyxPlay for Android <- Artemis (files live below android/)
git fetch artemis moonlight-noir
git merge -X subtree=android/ artemis/moonlight-noir
```

After a pull:

1. Re-apply submodule changes: compare `host/.gitmodules` / `pc/.gitmodules` with the root
   `.gitmodules` (git only reads the root file) and update the gitlinks with
   `git update-index --cacheinfo 160000,<sha>,<path>`.
2. Vendored `moonlight-common-c` copies are not updated automatically. Merge upstream changes manually;
   NyxPlay changes are marked with `NyxPlay` comments and listed in each `VENDORED.md`.
3. Search for new user visible "Apollo", "Artemis" or "Moonlight" strings and keep the NyxPlay names
   (the web UI locale files, Android `strings.xml` and the Qt `.ts` files).
4. Run the test suites described in [TESTING.md](TESTING.md).

## License obligations

All upstream projects are GPL-3.0. Keep the upstream `LICENSE`/`NOTICE` files, mention the upstream
authors in [CREDITS.md](../CREDITS.md) and publish the complete corresponding source for every binary
release.
