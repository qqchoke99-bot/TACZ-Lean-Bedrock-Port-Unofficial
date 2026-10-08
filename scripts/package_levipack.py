import argparse, json, re, sys, zipfile
from pathlib import Path

VALUE_PATTERN = re.compile(
    r'^\s*inline\s+constexpr\s+std::string_view\s+(Name|Author|Description|Version)\s*=\s*"((?:\\.|[^"\\])*)";\s*$'
)
REQUIRED = ("Name", "Author", "Description", "Version")
LIBRARY_NAME = "libTaczLean.so"

def parse_version(path: Path) -> dict:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = VALUE_PATTERN.match(line)
        if m:
            values[m.group(1)] = bytes(m.group(2), "utf-8").decode("unicode_escape")
    missing = [n for n in REQUIRED if not values.get(n)]
    if missing:
        raise ValueError("Missing version metadata: " + ", ".join(missing))
    return values

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--icon", required=True)
    ap.add_argument("--version-header", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--buttons-dir", default="")
    args = ap.parse_args()

    lib, icon, ver, out = Path(args.library), Path(args.icon), Path(args.version_header), Path(args.output)
    values = parse_version(ver)
    manifest = {
        "type": "preload-native",
        "name": values["Name"],
        "author": values["Author"],
        "description": values["Description"],
        "version": values["Version"],
        "entry": LIBRARY_NAME,
        "icon": "icon.png",
        "overwrite_files": ["icon.png"],
        "overwrite_folders": [],
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
        z.write(lib, LIBRARY_NAME)
        z.write(icon, "icon.png")
        # bundle button assets for user to copy
        buttons = Path(args.buttons_dir) if args.buttons_dir else Path("assets/buttons")
        if buttons.is_dir():
            for p in buttons.glob("*.png"):
                z.write(p, f"buttons/{p.name}")
    print(f"Wrote {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
