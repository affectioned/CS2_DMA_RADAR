# AssetExtractor

Extracts CS2 radar images and weapon icons directly from the game's VPK files and outputs PNGs ready for use alongside CS2 Radar.

## Requirements

### 1. Python 3.10+
Must be on PATH. `run.bat` handles the rest (creates a venv, installs Python dependencies).

### 2. VRF Source2Viewer CLI
Used to decompile Source 2 compiled assets (`.vtex_c` textures and `.vsvg_c` SVGs) from the VPK.

1. Go to https://github.com/ValveResourceFormat/ValveResourceFormat/releases
2. Download **`cli-windows-x64.zip`** (self-contained, no .NET install required)
3. Extract the zip contents into:
   ```
   tools/AssetExtractor/vrf/
   ```
   The folder must contain `Source2Viewer-CLI.exe`. The script finds whichever `.exe` is present automatically.

The `vrf/` folder is gitignored — it is never committed to the repo.

## Usage

```
run.bat
```

On first run, `run.bat` creates a local `.venv\` and installs Python dependencies. Subsequent runs skip setup.

CS2 is located automatically via the Steam registry (including libraries on secondary drives).

**Pass arguments after `run.bat` the same as you would to the script:**

```
run.bat --out C:\path\to\bin\Release\textures --icon-size 256 -v
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `--cs2 <path>` | auto | CS2 install directory (the `Counter-Strike Global Offensive` folder) |
| `--out <path>` | `./textures` | Output root — produces `maps/` and `icons/` subfolders |
| `--icon-size <px>` | `128` | Weapon icon height in pixels |
| `--no-radars` | — | Skip radar image extraction |
| `--no-icons` | — | Skip weapon icon extraction |
| `-v` / `--verbose` | — | Print each file as it is extracted |
| `--list [filter]` | — | Dump VPK file paths (optional substring filter) then exit |

## Output structure

```
textures/
  maps/
    de_dust2_radar.png
    de_dust2_radar.txt    ← map bounds (pos_x, pos_y, scale)
    de_nuke_radar.png
    de_nuke_radar.txt
    ar_shoots_radar.png
    ar_shoots_radar.txt
    ...
  icons/
    ak47.png
    awp.png
    knife_karambit.png
    ...
```

Place the `textures/` folder alongside the `.exe` at runtime. The radar overlay loads all maps and icons automatically — no code changes needed when new maps or weapons are added, just re-run the extractor.

## Picking up Valve updates

Re-runs are incremental. Each extracted asset's bytes are compared against the file already on disk; only files whose contents actually changed are rewritten. The end-of-run summary tells you exactly what happened, e.g.:

```
Radars   : 64 total (1 new, 2 updated, 61 unchanged)  ->  ./textures/maps
Overviews: 64 total (0 new, 2 updated, 62 unchanged)  ->  ./textures/maps
Icons    : 52 total (0 new, 0 updated, 52 unchanged)  ->  ./textures/icons
```

This means after a CS2 patch you can just re-run `run.bat` — no need to delete the existing `textures/` folder. Use `-v` to see the per-file `new`/`updated`/`unchanged` status.

## How it works

1. Passes `game/csgo/pak01_dir.vpk` directly to VRF CLI, which reads and decompiles assets in one step.
2. Extracts all `.vtex_c` radar textures from `panorama/images/overheadmaps/`; VRF outputs `.png` files.
3. Normalises filenames to `<mapname>_radar.png` (e.g. `de_dust2_radar_psd.png` → `de_dust2_radar.png`). All map variants are included — arena maps, night versions, etc.
4. Extracts overview bounds from `resource/overviews/*.txt` inside the VPK (via the `vpk` Python library — these files are not on the filesystem). Renames them to `<mapname>_radar.txt`. The C++ side parses `pos_x`/`pos_y`/`scale` from these at startup to position players correctly on the radar — no hardcoded bounds.
5. Extracts `.vsvg_c` weapon icon SVGs from the equipment icons path and converts them to PNGs using `svglib`: renders white silhouettes onto black, then promotes luminance to alpha so icons are white-on-transparent.

## Adding to the Visual Studio solution

Right-click the solution → **Add** → **New Solution Folder** → name it `tools`. Then right-click that folder → **Add Existing Item** and select all files here. The `vrf/` and `.venv/` folders are local-only and not committed.
