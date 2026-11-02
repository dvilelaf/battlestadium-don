"""Write the per-variant allow-list manifest (config/disc_manifest.toml) from a disc folder or a plain ISO.

Only file names, sizes and SHA-256 digests are written, never game bytes (AGENTS.md rule 1).
The variant id is found by the SHA-1 of DATA.AFS against config/discs.toml, unless --variant is given.
An existing manifest keeps its other variants; the variant being written is replaced.

  uv run --no-project python -I tools/disc/make_disc_manifest.py <folder-or-iso> [--variant ID] [--out FILE]
"""
import argparse
import fnmatch
import hashlib
import re
import struct
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CHUNK = 1 << 20
SECTOR = 2048

# Same list as docs/tasks/M6-public-release.md, card T-622.
ALLOW_LIST = ["SYSTEM.CNF", "SLPS_256.75", "DIR.DIR", "DATA.AFS", "BGM.AFS", "SE.AFS", "MOVIE.AFS", "MODULES/*"]


class FolderSource:
    def __init__(self, root: Path):
        self.root = root

    def files(self):
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*") if p.is_file())

    def open(self, rel):
        return (self.root / rel).open("rb")

    def size(self, rel):
        return (self.root / rel).stat().st_size


class IsoSource:
    """Plain ISO9660 with 2048-byte sectors (the format extract_disc reads first)."""

    def __init__(self, path: Path):
        self.path = path
        self.entries = {}
        with path.open("rb") as f:
            f.seek(16 * SECTOR)
            pvd = f.read(SECTOR)
            if pvd[0] != 1 or pvd[1:6] != b"CD001":
                raise SystemExit(f"{path}: not an ISO9660 image")
            self._walk(f, struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0], "")

    def _walk(self, f, lba, size, prefix):
        f.seek(lba * SECTOR)
        buf = f.read(size)
        off = 0
        while off < len(buf):
            ln = buf[off]
            if ln == 0:
                off = (off // SECTOR + 1) * SECTOR
                continue
            nl = buf[off + 32]
            name = buf[off + 33:off + 33 + nl]
            rec_lba, rec_size = struct.unpack_from("<I", buf, off + 2)[0], struct.unpack_from("<I", buf, off + 10)[0]
            is_dir = buf[off + 25] & 2
            off += ln
            if nl == 1 and name in (b"\x00", b"\x01"):
                continue
            n = name.decode("ascii", "replace").split(";")[0].rstrip(".")
            rel = f"{prefix}{n}"
            if is_dir:
                self._walk(f, rec_lba, rec_size, rel + "/")
            else:
                self.entries[rel] = (rec_lba, rec_size)

    def files(self):
        return sorted(self.entries)

    def size(self, rel):
        return self.entries[rel][1]

    def open(self, rel):
        lba, size = self.entries[rel]
        f = self.path.open("rb")
        f.seek(lba * SECTOR)
        return _Limited(f, size)


class _Limited:
    def __init__(self, f, left):
        self.f, self.left = f, left

    def read(self, n):
        data = self.f.read(min(n, self.left))
        self.left -= len(data)
        return data

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.f.close()


def digest(src, rel, algo):
    h = hashlib.new(algo)
    with src.open(rel) as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
    return h.hexdigest()


def allowed(rel):
    return any(fnmatch.fnmatchcase(rel.upper(), pat.upper()) for pat in ALLOW_LIST)


def serial_of(src):
    with src.open("SYSTEM.CNF") as f:
        text = f.read(4096).decode("ascii", "replace")
    m = re.search(r"BOOT2\s*=\s*cdrom0:\\?([A-Z]{4})_(\d{3})\.(\d{2})", text, re.I)
    if not m:
        raise SystemExit("SYSTEM.CNF has no BOOT2 line with a serial")
    return f"{m[1].upper()}-{m[2]}{m[3]}"


def variant_id(src):
    sha1 = digest(src, "DATA.AFS", "sha1")
    with (REPO / "config" / "discs.toml").open("rb") as f:
        for v in tomllib.load(f).get("variant", []):
            if v["data_afs_sha1"] == sha1:
                return v["id"]
    raise SystemExit(f"DATA.AFS sha1 {sha1} is not in config/discs.toml; pass --variant")


def build_variant(src, vid):
    rels = [r for r in src.files() if allowed(r)]
    missing = [p for p in ALLOW_LIST if not p.endswith("*") and p not in rels]
    if missing:
        raise SystemExit(f"missing from the disc: {', '.join(missing)}")
    files = []
    for rel in rels:
        files.append({"path": rel, "size": src.size(rel), "sha256": digest(src, rel, "sha256")})
    return {"id": vid, "serial": serial_of(src), "file": files}


def emit(variants):
    out = ["# Allow-list of the files a player's disc may contain: path, size and SHA-256 per variant.",
           "# Written by tools/disc/make_disc_manifest.py. Hashes and sizes only, no game data.", ""]
    for v in variants:
        out += ["[[variant]]", f'id = "{v["id"]}"', f'serial = "{v["serial"]}"', ""]
        for f in v["file"]:
            out += ["[[variant.file]]", f'path = "{f["path"]}"', f'size = {f["size"]}', f'sha256 = "{f["sha256"]}"', ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", type=Path, help="extracted disc folder or plain .iso")
    ap.add_argument("--variant", help="variant id (default: found through DATA.AFS in config/discs.toml)")
    ap.add_argument("--out", type=Path, default=REPO / "config" / "disc_manifest.toml")
    a = ap.parse_args()
    src = FolderSource(a.source) if a.source.is_dir() else IsoSource(a.source)
    new = build_variant(src, a.variant or variant_id(src))
    variants = []
    if a.out.exists():
        with a.out.open("rb") as f:
            variants = [v for v in tomllib.load(f).get("variant", []) if v["id"] != new["id"]]
    variants.append(new)
    a.out.write_text(emit(variants), encoding="utf-8", newline="\n")
    print(f"wrote {a.out}: variant {new['id']} ({new['serial']}), {len(new['file'])} files, "
          f"{sum(f['size'] for f in new['file'])} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
