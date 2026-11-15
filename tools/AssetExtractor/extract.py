#!/usr/bin/env python3
"""
CS2 Asset Extractor
Extracts radar images and weapon icons from CS2 VPK files and outputs PNGs
ready for use alongside CS2 Radar (place the output textures/ folder
next to the .exe).

All VPK I/O goes through Source2Viewer-CLI (tools/AssetExtractor/vrf/) — we
drive the CLI and read its outputs, so decoding stays in sync with Valve's
latest format changes. Python-side work is limited to driving the CLI,
rasterizing CLI-produced SVGs to PNGs, and incremental byte-compare writes.

Requires VRF Source2Viewer-CLI.exe in tools/AssetExtractor/vrf/ — see README.md.
"""

import io
import re
import subprocess
import sys
import tempfile
import argparse
from pathlib import Path


# ── Dependency checks ──────────────────────────────────────────────────────────

def _require(import_name, pip_name=None):
    import importlib
    try:
        return importlib.import_module(import_name)
    except ImportError:
        sys.exit(f"Missing dependency: pip install {pip_name or import_name}")

svglib   = _require("svglib.svglib", pip_name="svglib")
renderPM = _require("reportlab.graphics.renderPM", pip_name="reportlab")
PIL      = _require("PIL", pip_name="Pillow")


# ── CS2 path detection ─────────────────────────────────────────────────────────

def _steam_library_paths(steam_root: Path) -> list[Path]:
    paths = [steam_root]
    vdf = steam_root / "steamapps" / "libraryfolders.vdf"
    if not vdf.exists():
        return paths
    text = vdf.read_text(encoding="utf-8", errors="ignore")
    for m in re.finditer(r'"path"\s+"([^"]+)"', text):
        p = Path(m.group(1).replace("\\\\", "\\"))
        if p not in paths:
            paths.append(p)
    return paths


def find_cs2() -> Path | None:
    steam_roots: list[Path] = []
    if sys.platform == "win32":
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r"SOFTWARE\WOW6432Node\Valve\Steam")
            steam_roots.append(Path(winreg.QueryValueEx(key, "InstallPath")[0]))
            winreg.CloseKey(key)
        except Exception:
            pass
    steam_roots += [
        Path(r"C:\Program Files (x86)\Steam"),
        Path(r"C:\Program Files\Steam"),
        Path.home() / ".steam/steam",
    ]
    cs2_rel = Path("steamapps") / "common" / "Counter-Strike Global Offensive"
    for root in steam_roots:
        for lib in _steam_library_paths(root):
            candidate = lib / cs2_rel
            if candidate.exists():
                return candidate
    return None


# ── Incremental write ──────────────────────────────────────────────────────────
#
# Compares new bytes against the file already on disk so re-runs only touch
# files whose content has actually changed. Lets the extractor pick up Valve's
# map/icon updates without forcing a full delete-and-re-extract.

def _write_if_changed(path: Path, data: bytes) -> str:
    """Write bytes to path only if content differs. Returns 'new', 'updated', or 'unchanged'."""
    if path.exists():
        if path.read_bytes() == data:
            return "unchanged"
        path.write_bytes(data)
        return "updated"
    path.write_bytes(data)
    return "new"


# ── Source2Viewer-CLI driver ───────────────────────────────────────────────────

def _find_cli() -> Path:
    vrf_dir = Path(__file__).parent / "vrf"
    exes = list(vrf_dir.glob("*.exe")) if vrf_dir.exists() else []
    if exes:
        return exes[0]
    sys.exit(
        f"VRF CLI not found in: {vrf_dir}\n"
        "Download cli-windows-x64.zip from:\n"
        "  https://github.com/ValveResourceFormat/ValveResourceFormat/releases\n"
        "Extract the contents into: tools/AssetExtractor/vrf/"
    )


def _s2v(cli: Path, vpk_path: Path, out_dir: Path, extension: str,
         vpk_prefix: str, decompile: bool, verbose: bool) -> None:
    """
    Run Source2Viewer-CLI to extract files from a VPK.
    -i  input VPK
    -o  output directory (CLI mirrors VPK path structure under it)
    -d  decompile — needed for vtex_c/vsvg_c/etc.; omit for raw files like .txt
    -e  filter by extension inside the VPK
    -f  filter by path prefix inside the VPK
    """
    cmd = [str(cli), "-i", str(vpk_path), "-o", str(out_dir),
           "-e", extension,
           "-f", vpk_prefix,
           "--quiet"]
    if decompile:
        cmd.append("-d")
    if verbose:
        print(f"  [s2v]    {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.stderr.strip():
        print(result.stderr.rstrip(), file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"Source2Viewer-CLI exited {result.returncode}")


# ── SVG bytes → PNG bytes ──────────────────────────────────────────────────────
#
# CS2 weapon icons are white silhouettes on a transparent background.
# Source2Viewer-CLI decodes .vsvg_c to .svg but there's no CLI path to
# rasterize to PNG, so svglib/reportlab do the final step. Render onto
# black, then promote luminance to alpha:
#   black pixel (background) → luminance 0   → fully transparent
#   white pixel (icon)       → luminance 255 → fully opaque

def _svg_to_png(svg_bytes: bytes, height: int) -> bytes:
    from PIL import Image

    drawing = svglib.svg2rlg(io.BytesIO(svg_bytes))
    if drawing is None:
        raise ValueError("svglib could not parse SVG")
    if drawing.height > 0:
        scale = height / drawing.height
        drawing.width  *= scale
        drawing.height *= scale
        drawing.transform = (scale, 0, 0, scale, 0, 0)

    raw = renderPM.drawToString(drawing, fmt="PNG", bg=0x000000)

    img = Image.open(io.BytesIO(raw)).convert("RGBA")
    img.putalpha(img.convert("L"))

    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


# ── Overview txt extraction ────────────────────────────────────────────────────

_OVERVIEW_VPK_PREFIX = "resource/overviews/"

def extract_overview_txts(cli: Path, vpk_path: Path, out_dir: Path,
                          verbose: bool) -> tuple[int, int, int]:
    """Extract *_radar.txt bounds files from the VPK (resource/overviews/*.txt).
    Files are renamed <mapname>_radar.txt to match the PNG naming convention.
    Returns (new, updated, unchanged) counts."""
    out_dir.mkdir(parents=True, exist_ok=True)
    new_n = upd_n = unc_n = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp_out = Path(tmp) / "out"
        tmp_out.mkdir()

        # overviews are plain .txt — no decompile step.
        _s2v(cli, vpk_path, tmp_out, "txt", _OVERVIEW_VPK_PREFIX,
             decompile=False, verbose=verbose)

        for txt in sorted(tmp_out.rglob("*.txt")):
            stem     = txt.stem               # e.g. "de_dust2"
            out_name = stem + "_radar.txt"    # e.g. "de_dust2_radar.txt"
            try:
                status = _write_if_changed(out_dir / out_name, txt.read_bytes())
                if   status == "new":     new_n += 1
                elif status == "updated": upd_n += 1
                else:                     unc_n += 1
                if verbose:
                    print(f"  [overview] {status:9s} {txt.name}  ->  {out_name}")
            except Exception as e:
                print(f"  [overview] FAILED {txt.name}: {e}", file=sys.stderr)
    return new_n, upd_n, unc_n


# ── Radar extraction ───────────────────────────────────────────────────────────

_RADAR_VPK_PREFIX = "panorama/images/overheadmaps/"

def extract_radars(cli: Path, vpk_path: Path, out_dir: Path, verbose: bool) -> tuple[int, int, int]:
    """Extract radar PNGs. Returns (new, updated, unchanged) counts."""
    out_dir.mkdir(parents=True, exist_ok=True)
    new_n = upd_n = unc_n = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp_out = Path(tmp) / "out"
        tmp_out.mkdir()

        _s2v(cli, vpk_path, tmp_out, "vtex_c", _RADAR_VPK_PREFIX,
             decompile=True, verbose=verbose)

        # CLI preserves VPK path structure under tmp_out, so use rglob.
        # Sort so that for maps with multiple variants (e.g. de_cache_radar_psd
        # AND de_cache_radar_tga), the alphabetically-later variant wins the
        # dedupe below — preserves the historical "_tga overwrites _psd"
        # behaviour without writing the same destination twice per run.
        candidates = sorted(list(tmp_out.rglob("*.png")) + list(tmp_out.rglob("*.tga")))
        if not candidates:
            print("  [radar]  WARNING: CLI produced no image output for radar textures",
                  file=sys.stderr)

        # Dedupe by destination filename: later candidates overwrite earlier
        # ones in the dict, so e.g. _tga wins over _psd.
        best: dict[str, Path] = {}
        for img_file in candidates:
            stem = img_file.stem
            if "_radar" not in stem:
                continue
            # de_dust2_radar_psd → de_dust2_radar.png
            out_name = re.sub(r"_radar.*$", "_radar", stem) + ".png"
            best[out_name] = img_file

        for out_name, img_file in best.items():
            status = _write_if_changed(out_dir / out_name, img_file.read_bytes())
            if   status == "new":     new_n += 1
            elif status == "updated": upd_n += 1
            else:                     unc_n += 1
            if verbose:
                print(f"  [radar]  {status:9s} {img_file.name}  ->  {out_name}")

    return new_n, upd_n, unc_n


# ── Weapon icon extraction ─────────────────────────────────────────────────────

_ICON_VPK_PREFIX = "panorama/images/icons/equipment/"

_NAME_OVERRIDES: dict[str, str] = {
    "p2000": "hkp2000",
}

_ALIASES: dict[str, str] = {
    "m4a1_silencer_off": "m4a1_silencer",
    "usp_silencer_off":  "usp_silencer",
}

def extract_icons(cli: Path, vpk_path: Path, out_dir: Path, icon_height: int, verbose: bool) -> tuple[int, int, int]:
    """Extract weapon icon PNGs. Returns (new, updated, unchanged) counts."""
    out_dir.mkdir(parents=True, exist_ok=True)
    extracted: dict[str, Path] = {}
    new_n = upd_n = unc_n = 0

    def _tally(status: str) -> None:
        nonlocal new_n, upd_n, unc_n
        if   status == "new":     new_n += 1
        elif status == "updated": upd_n += 1
        else:                     unc_n += 1

    with tempfile.TemporaryDirectory() as tmp:
        tmp_out = Path(tmp) / "out"
        tmp_out.mkdir()

        _s2v(cli, vpk_path, tmp_out, "vsvg_c", _ICON_VPK_PREFIX,
             decompile=True, verbose=verbose)

        # Dedupe by destination — multiple SVGs can map to the same out_stem via
        # _NAME_OVERRIDES (e.g. both p2000.svg and hkp2000.svg → hkp2000.png).
        # Sort first so the alphabetically-later source wins in a stable order.
        best: dict[str, Path] = {}
        for svg in sorted(tmp_out.rglob("*.svg")):
            stem     = svg.stem
            out_stem = _NAME_OVERRIDES.get(stem, stem)
            best[out_stem] = svg

        for out_stem, svg in best.items():
            out_path = out_dir / f"{out_stem}.png"
            try:
                png_bytes = _svg_to_png(svg.read_bytes(), icon_height)
                status = _write_if_changed(out_path, png_bytes)
                _tally(status)
                extracted[out_stem] = out_path
                if verbose:
                    print(f"  [icon]   {status:9s} {svg.name}  ->  {out_stem}.png")
            except Exception as e:
                print(f"  [icon]   FAILED {svg.name}: {e}", file=sys.stderr)

    for alias, source in _ALIASES.items():
        if alias not in extracted and source in extracted:
            dst = out_dir / f"{alias}.png"
            status = _write_if_changed(dst, extracted[source].read_bytes())
            _tally(status)
            extracted[alias] = dst
            if verbose:
                print(f"  [icon]   {status:9s} {alias}.png  (copy of {source}.png)")

    return new_n, upd_n, unc_n


# ── VPK listing (via CLI) ──────────────────────────────────────────────────────

def list_vpk(cli: Path, vpk_path: Path, substr: str) -> None:
    """Print VPK contents matching a substring (case-insensitive), via Source2Viewer-CLI."""
    cmd = [str(cli), "-i", str(vpk_path), "--vpk_list"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr.rstrip(), file=sys.stderr)
        sys.exit(result.returncode)
    needle = substr.lower()
    for line in result.stdout.splitlines():
        if not needle or needle in line.lower():
            print(line)


# ── Entry point ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Extract CS2 radar images and weapon icons from game VPK files."
    )
    parser.add_argument("--cs2",       metavar="PATH", help="CS2 install directory (auto-detected if omitted)")
    parser.add_argument("--out",       metavar="PATH", default="textures", help="Output root (default: ./textures)")
    parser.add_argument("--icon-size", metavar="PX",   type=int, default=128, help="Weapon icon height in pixels (default: 128)")
    parser.add_argument("--no-radars", action="store_true", help="Skip radar extraction")
    parser.add_argument("--no-icons",  action="store_true", help="Skip weapon icon extraction")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print each file as it is extracted")
    parser.add_argument("--list", metavar="FILTER", nargs="?", const="",
                        help="Dump VPK paths (optionally filtered by substring) then exit")
    args = parser.parse_args()

    cs2 = Path(args.cs2) if args.cs2 else find_cs2()
    if not cs2 or not cs2.exists():
        sys.exit("Could not find CS2. Pass --cs2 <path> pointing to the 'Counter-Strike Global Offensive' folder.")
    print(f"CS2 path : {cs2}")

    vpk_path = cs2 / "game" / "csgo" / "pak01_dir.vpk"
    if not vpk_path.exists():
        sys.exit(f"VPK not found: {vpk_path}")
    print(f"VPK      : {vpk_path.name}")

    cli = _find_cli()

    if args.list is not None:
        list_vpk(cli, vpk_path, args.list)
        return

    out_root = Path(args.out)
    print(f"Output   : {out_root.resolve()}\n")

    def _summary(label: str, counts: tuple[int, int, int], dest: Path) -> None:
        new_n, upd_n, unc_n = counts
        total = new_n + upd_n + unc_n
        print(f"{label}: {total} total ({new_n} new, {upd_n} updated, {unc_n} unchanged)  ->  {dest}")

    if not args.no_radars:
        _summary("Radars   ", extract_radars(cli, vpk_path, out_root / "maps", args.verbose),            out_root / "maps")
        _summary("Overviews", extract_overview_txts(cli, vpk_path, out_root / "maps", args.verbose),     out_root / "maps")

    if not args.no_icons:
        _summary("Icons    ", extract_icons(cli, vpk_path, out_root / "icons", args.icon_size, args.verbose), out_root / "icons")

    print("\nDone.")


if __name__ == "__main__":
    main()
