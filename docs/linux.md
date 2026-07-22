# Linux and Steam Deck (Proton)

The shipped port is a **Windows executable** (`nhllegacy.exe`) built with a Vulkan
backend (`win-amd64-vk-pgo`). On Linux it runs through **Steam + Proton**, not as a
native ELF binary.

Community testing (independent of the project maintainers) has confirmed this works on:

- **Pop!_OS** — NVIDIA GeForce RTX 3080, driver 580.159.03, Steam (deb install), Proton 11.0
- **Steam Deck** — Proton 11.0, non-Steam shortcut to `nhllegacy.exe`

You must own NHL Legacy and provide your own legally dumped copy of the game. See
[README.md](../README.md) for legal requirements.

## Two paths

| You have… | What to do |
|-----------|------------|
| A completed install folder (`nhllegacy.exe` + `game/`) | [Flow 1 — Play with Steam](#flow-1-play-an-existing-install-with-steam) |
| A [release zip](https://github.com/puckhead73/nhl-legacy-recomp/releases) and a disc `.iso` | [Flow 2 — Assemble an install on Linux](#flow-2-assemble-an-install-on-linux) |

**Note:** The release zip contains prebuilt port binaries in `payload/`. The builder
validates your dump, extracts game files, and lays down those binaries — it does
**not** run the static recompilation pipeline. Compiling the port from source still
requires a Windows development setup; see [DEV-README.md](../DEV-README.md).

---

## Flow 1: Play an existing install with Steam

Use this when you already have a working install (for example, one built on Windows
or copied from another machine).

### Requirements

- Linux with a Vulkan-capable GPU and up-to-date drivers
- [Steam](https://store.steampowered.com/) with **Proton 11.0** (Proton Experimental also works)
- An install folder containing at least `nhllegacy.exe`, `rexruntime.dll`,
  `amd_fidelityfx_vk.dll`, and `game/`

### Steps

1. In Steam: **Add a Game → Add a Non-Steam Game → Browse**, then select `nhllegacy.exe`
   inside your install folder.
2. Open the new library entry → **Properties → Compatibility**.
3. Enable **Force the use of a specific Steam Play compatibility tool** and choose
   **Proton 11.0**.
4. Optional launch options: `PROTON_USE_WINED3D=0 %command%` (keeps the Vulkan path;
   recommended on NVIDIA).
5. Launch from your library.

No extra configuration is required beyond Proton. Xbox controllers generally work
out of the box.

### Install folder layout

```
NHL Legacy Recomp/
├── nhllegacy.exe
├── rexruntime.dll
├── amd_fidelityfx_vk.dll
├── nhl_enhancements.ini   # optional graphics settings
├── tunables_schema.tsv
├── logs/                  # runtime logs (useful when troubleshooting)
└── game/                  # dumped Xbox 360 assets (~10 GB)
```

Saves and shader cache live inside the Proton prefix, typically under:

```
~/.steam/steam/steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/Documents/nhllegacy/
```

### Graphics settings

Edit `nhl_enhancements.ini` beside `nhllegacy.exe`:

```ini
supersampling=2
fullscreen=1
vsync=0
ffx_effect=cas
```

On Wayland or if fullscreen misbehaves, try `fullscreen=0`.

### Troubleshooting

| Symptom | What to try |
|---------|-------------|
| Game exits immediately | Confirm the shortcut targets `nhllegacy.exe` and Proton 11 is forced in Properties |
| Missing `MSVCP140.dll` / `VCRUNTIME140.dll` | Switch to Proton Experimental, or install `vcrun2019` via winetricks in the game's prefix |
| Black screen | Set `fullscreen=0`; keep `PROTON_USE_WINED3D=0` |
| Many `cache:\` / `update:\` errors in logs | Normal — the Xbox 360 title probes optional paths that do not exist on PC |

After each launch, check the newest `logs/nhllegacy_*.log`. A successful Linux run
shows the host Vulkan driver (for example `libGLX_nvidia.so.0` on NVIDIA), not
`nvoglv64.dll`.

---

## Flow 2: Assemble an install on Linux

Use this when you have a **release zip** and your own NHL Legacy `.iso`, but no
completed install yet.

The source repository alone is not enough: you need the release package, which
includes `nhl-legacy-builder.exe` and the prebuilt `payload/` directory (port
binaries, manifest, and archive extractors).

### Requirements

- Same as [Flow 1](#flow-1-play-an-existing-install-with-steam), plus:
- A [release zip](https://github.com/puckhead73/nhl-legacy-recomp/releases) extracted
  with `payload/` kept next to `nhl-legacy-builder.exe`
- Your own raw Xbox 360 disc dump (`.iso`) or an extracted folder containing
  `default.xex` at the root
- ~10 GB free space for the output install

### Steps

1. Install Steam and Proton 11.0 if you have not already.
2. Extract the release zip, for example to `~/nhl-legacy-recomp-0.4.0/`.
3. Verify your dump (optional, takes a few seconds):

   ```bash
   export STEAM_DIR="$HOME/.steam/debian-installation"   # adjust if your Steam path differs
   RELEASE=~/nhl-legacy-recomp-0.4.0
   ISO=~/dumps/NHL\ Legacy.iso

   cd "$RELEASE"
   "$STEAM_DIR/steamapps/common/Proton 11.0/proton" run ./nhl-legacy-builder.exe \
     verify --iso "$ISO"
   ```

4. Assemble the install (~1–2 minutes depending on disk speed):

   ```bash
   OUT=~/Games/NHL\ Legacy\ Recomp

   "$STEAM_DIR/steamapps/common/Proton 11.0/proton" run ./nhl-legacy-builder.exe \
     install --iso "$ISO" --out "$OUT"
   ```

   Use `--from /path/to/extracted-folder` instead of `--iso` if you already have an
   extracted game tree.

5. Follow [Flow 1](#flow-1-play-an-existing-install-with-steam) to add
   `$OUT/nhllegacy.exe` to Steam and launch with Proton 11.0.

Native Linux paths work for `--iso` and `--out`. If a path is not found, try Wine
style paths (`Z:\home\you\...`) instead.

### Builder troubleshooting

- **"default.xex does not match the supported build"** — the port targets one
  specific vanilla disc image. A different region, modified dump, or title-updated
  image will not pass validation.
- **"not a recognized Xbox 360 disc image"** — re-rip the disc to a plain raw
  `.iso` (XDVDFS).
- **Install interrupted** — re-run the same `install` command; it will redo extraction.

---

## Command-line launch (without the Steam UI)

If you prefer not to use the Steam library shortcut:

```bash
export STEAM_DIR="$HOME/.steam/debian-installation"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_DIR"
export STEAM_COMPAT_DATA_PATH="$STEAM_DIR/steamapps/compatdata/<your-appid>"
cd "/path/to/NHL Legacy Recomp"
PROTON_USE_WINED3D=0 "$STEAM_DIR/steamapps/common/Proton 11.0/proton" run ./nhllegacy.exe
```

`STEAM_COMPAT_DATA_PATH` can point at any compatdata folder; Steam assigns one when
you add a non-Steam game.
