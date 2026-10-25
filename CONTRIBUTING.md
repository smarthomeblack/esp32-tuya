# Contributing

Thanks for wanting to contribute to the project! Every change goes through a pull request so CI can check it and a maintainer can review it before it lands in `master`.

## Dev environment

Install ESP-IDF v6.0.1 and build the firmware from source: **[docs/getting-started.md](docs/getting-started.md)** — it has the ESP-IDF install links for each operating system plus every build/flash step.

## Architecture

Before changing code, read **[docs/architecture.md](docs/architecture.md)**: the component map, where the wiring hooks live, the main event loop rules (Schedule/ScheduleCritical, REST marshalling), the self-test gate at boot, and the web UI card render pipeline. Most PRs touch exactly those areas.

## PR workflow

1. Create a `feature/xxx` branch from `master`:

   ```powershell
   git switch -c feature/xxx
   ```

2. Code + commit (build clean locally first — the one-line command is in the [README](README.md)).
3. Push + open a PR into `master`:

   ```powershell
   git push -u origin feature/xxx
   gh pr create --base master   # or the "New pull request" button on the GitHub web UI
   ```

4. **CI must be green** — if it fails, fix the issue and push more commits (the list of CI steps is below).
5. The maintainer reviews and merges — respond to any review comments by pushing commits to your branch. After the merge you can delete your branch (the Delete branch button on the GitHub web UI).

## Code rules

- **Format C++ with clang-format** — the repo's version pin is 23.1.3. CI blocks PRs with wrong formatting: run clang-format on the files you changed before pushing.
- **Keep comments short and in English** — only state what the code doesn't show at a glance (units, protocol constraints...), no line-by-line narration.
- **The web UI is ES modules** — one file per concern, never merged into one big file.

## Web UI

After changing `components/web_assets/src/`:

```bash
python scripts/build_web.py          # packs src → dist (image assets are embedded into the firmware)
python scripts/check_web_imports.py  # import-graph lint — CI runs this exact step
```

Two mandatory UI rules:

- Every option list (running mode, fan speed, floor level...) uses a `<select>` dropdown, never a row of buttons/chips.
- Every enabled entity must be visible on its device card, even when it has no value yet.

## Testing

Read [docs/testing.md](docs/testing.md) — all the self-test gates, the mock server, and golden vectors.

- **Firmware changes**: must pass the full boot self-test on real hardware before you send the PR (949/949 checks — a failed self-test keeps the hub off the network; details in docs/testing.md).
- **Web UI changes**: QA on the mock server (`python scripts/mock_server.py`) — no real hub needed.
- **Regenerating golden vectors** (`python scripts/gen_test_vectors.py`) needs the repo's one Python dependency: `pip install -r scripts/requirements.txt`.

## What CI runs on your pull request

Every PR into `master` goes through 4 steps:

- Build web assets (`python scripts/build_web.py`).
- Lint the web UI import graph (`python scripts/check_web_imports.py`).
- Check C++ formatting with clang-format 23.1.3.
- Build the firmware in an ESP-IDF v6.0.1 container with `-Werror` — any warning fails.

## Releases

Releases are cut by the maintainer; the `VERSION` file and release tags are not part of pull requests.

## Other rules

- **Firmware changes must build clean with `-Werror`** — ESP-IDF v6 defaults to warnings-as-errors, and CI catches every warning.
- Don't commit `build/`, `sdkconfig`, `managed_components/`, `dist/` — they are already in `.gitignore`.

## Documentation

- [docs/api.md](docs/api.md) — the full REST/WS contract (endpoints, events, error tables).
- [docs/testing.md](docs/testing.md) — self-test gates + mock server; develop the UI without a real hub.
- [docs/getting-started.md](docs/getting-started.md) — install ESP-IDF, build, flash firmware.
- [docs/architecture.md](docs/architecture.md) — component map, thread flow, self-test gate, web UI pipeline.
