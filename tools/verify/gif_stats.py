#!/usr/bin/env python3
"""Describe the GIF packets of one frame: what the GS was asked to draw (T-202 step 1).

    uv run --no-project python -I tools/verify/gif_stats.py <file> [--frame N] [--tags] [--json FILE]

Two inputs are accepted:

* a PCSX2 GS dump (`work/gsdump/01-title.gs.zst`; the container is documented in
  `docs/knowledge/15-gs-dumps.md`). Careful: PCSX2 labels every transfer with index 3 (Path1New), so the
  dump does **not** say whether a packet came from the VU1 (path 1), from VIF1 direct (path 2) or from
  the GIF channel (path 3). The tool prints that index as `path(dump index)` and says so.
* the port's own capture (`DON_GS_PACKETS`, `src/host/gs_trace.h`): the magic `DONGS1\\n`, a u32 frame
  number, then one record per transfer: `path u8, size u32, bytes`. There the path is the runtime's real
  one (`src/host/gs_glue.cpp`'s `gif_transfer(path, ...)`: 1 = VU1/XGKICK, 2 = VIF1 direct, 3 = GIF).

For every GIF tag the tool reports the tag fields (NLOOP, EOP, PRE, PRIM, FLG, NREG, REGS), the primitive
class, the vertex count, whether texture mapping, fog and alpha blending are on, the texture in use
(TEX0: TBP0, TBW, PSM, TWxTH, TCC, TFX, CBP) and the bounding box of the vertex positions in pixels
(after removing XYOFFSET; both are tracked from the packet stream, as the GS would) and the range of Z.
`--tags` prints every tag; without it a summary per path, per primitive class and per texture is printed.

The packets are the GS's input, not the picture: a tag whose texture was never uploaded, whose vertices
are all at one point, or that is drawn with the wrong colour is visible here and not in a screenshot.
This is a diagnostic, not a model of the GS: it walks PACKED and REGLIST tags and counts IMAGE data, it
does not emulate the rasteriser.

Only the standard library is used (`.zst` needs the `zstd` tool, as `dump_regs.py` does). The dumps and
the captures are game data: they stay under work/ (AGENTS.md rule 1); the figures may be quoted.
"""
from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

# ---- GIF constants (PCSX2: pcsx2/GS/GSRegs.h, GSState.cpp GIFPackedPath) --------------------------

NLOOP_MASK = 0x7FFF
EOP_BIT = 15
PRE_BIT = 46
PRIM_SHIFT = 47
FLG_SHIFT = 58
NREG_SHIFT = 60
FLG_PACKED = 0
FLG_REGLIST = 1
FLG_IMAGE = 2
FLG_DISABLED = 3

PRIM_CLASSES = {0: 'point', 1: 'line', 2: 'line_strip', 3: 'triangle', 4: 'triangle_strip',
                5: 'triangle_fan', 6: 'sprite', 7: 'reserved'}

# Registers that can appear in a PACKED register list (GS manual 7.1.1; PCSX2 GIF_REG_*).
REG_NAMES = {
    0x00: 'PRIM', 0x01: 'RGBAQ', 0x02: 'STQ', 0x03: 'UV', 0x04: 'XYZF2', 0x05: 'XYZ2', 0x06: 'TEX0_1',
    0x07: 'TEX0_2', 0x08: 'CLAMP_1', 0x09: 'CLAMP_2', 0x0A: 'FOG', 0x0B: 'NOP', 0x0C: 'XYZF3',
    0x0D: 'XYZ3', 0x0E: 'A+D', 0x0F: 'NOP',
}
# In REGLIST mode PCSX2 gives ids 0x00/0x01 to the three-qword vertex formats (STQ, RGBAQ, XYZ) and
# 0x04 to A+D: pcsx2/GS/GSRegs.h, GIF_REG_STQRGBAXYZF2 = 0, GIF_REG_STQRGBAXYZ2 = 1, GIF_REG_A_D = 0x0e.
REGLIST_VERTEX3 = {0x00: 'XYZF2', 0x01: 'XYZ2'}

PORT_MAGIC = b'DONGS1\n'


class GifStatsError(Exception):
    pass


# ---- tags -----------------------------------------------------------------------------------------

@dataclass
class Tag:
    """One GIF tag and the vertices it produced."""
    transfer: int
    path: int
    nloop: int
    eop: int
    pre: int
    prim: int
    flg: int
    nreg: int
    regs: list[int]
    qwords: int
    image_qwords: int = 0
    vertices: int = 0
    xy_min: tuple[float, float] | None = None
    xy_max: tuple[float, float] | None = None
    z_min: int | None = None
    z_max: int | None = None
    st: bool = False
    tex0: dict | None = None

    @property
    def prim_class(self) -> str:
        return PRIM_CLASSES.get(self.prim & 7, '?')

    @property
    def tme(self) -> int:
        return (self.prim >> 4) & 1

    @property
    def fge(self) -> int:
        return (self.prim >> 5) & 1

    @property
    def abe(self) -> int:
        return (self.prim >> 6) & 1

    def to_dict(self) -> dict:
        out = {'transfer': self.transfer, 'path': self.path, 'nloop': self.nloop, 'eop': self.eop,
               'pre': self.pre, 'prim_raw': self.prim, 'prim_class': self.prim_class, 'tme': self.tme,
               'fge': self.fge, 'abe': self.abe, 'flg': self.flg, 'nreg': self.nreg,
               'regs': [f'{r:02x}' for r in self.regs], 'qwords': self.qwords, 'vertices': self.vertices,
               'st': self.st}
        if self.image_qwords:
            out['image_qwords'] = self.image_qwords
        if self.xy_min is not None:
            out['xy_px_min'] = [round(v, 2) for v in self.xy_min]
            out['xy_px_max'] = [round(v, 2) for v in self.xy_max]
        if self.z_min is not None:
            out['z_min'] = self.z_min
            out['z_max'] = self.z_max
        if self.tex0 is not None:
            out['tex0'] = self.tex0
        return out


def tex0_fields(value: int) -> dict:
    """The fields of a TEX0 register (GS manual 7.1.2; PCSX2 GIFRegTEX0)."""
    return {
        'tbp0_bytes': (value & 0x3FFF) * 256,   # texture base pointer, in bytes
        'tbw': ((value >> 14) & 0x3F) * 64,     # buffer width, in pixels
        'psm': (value >> 20) & 0x3F,            # pixel storage format
        'tw': 1 << ((value >> 26) & 0xF),       # width in pixels (a power of two)
        'th': 1 << ((value >> 30) & 0xF),       # height in pixels
        'tcc': (value >> 34) & 1,               # 0 = RGB, 1 = RGBA
        'tfx': (value >> 35) & 3,               # 0 modulate, 1 decal, 2 highlight, 3 highlight2
        'cbp_bytes': ((value >> 37) & 0x3FFF) * 256,
        'cpsm': (value >> 51) & 0xF,
    }


def _signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


# The A+D qword and the vertex qwords use the same field layout (measured on the console's dumps and
# confirmed by PCSX2's GIFPackedXYZF2/GIFPackedXYZ2 structs in pcsx2/GS/GSRegs.h):
#   byte 0-1  X    (16-bit signed, 1/16 pixel)
#   byte 2-3  padding
#   byte 4-5  Y    (16-bit signed, 1/16 pixel)
#   byte 6-7  padding | Z (XYZF2: bits 64-91 = (u32 at byte 8) >> 4 & 0xFFFFFF; XYZ2: bits 64-95)
#   byte 8    the A+D register address (only in A+D qwords)
XYZF2 = 'XYZF2'
XYZ2 = 'XYZ2'


class GifParser:
    """Tracks the drawing registers a frame sets and turns its qwords into tags with vertices."""

    def __init__(self, default_xyoffset: int = 32768):
        self.prim = 0
        self.tex0_1 = 0
        self.tex0_2 = 0
        self.context = 1
        self.xyoffset = (default_xyoffset, default_xyoffset)
        self.xyoffset_seen = False
        self.alpha = 0
        self.test = 0
        self.relist_vertex_index = 0

    def current_tex0(self) -> int:
        return self.tex0_2 if self.context == 2 else self.tex0_1

    def write_register(self, addr: int, data: bytes) -> None:
        """A register write: the data is the low 64 bits, fields in the packed layout."""
        value = int.from_bytes(data[:8], 'little')
        if addr == 0x00:
            self.prim = value & 0x7FF
            self.context = 2 if (value >> 9) & 1 else 1
        elif addr == 0x06:
            self.tex0_1 = value
        elif addr == 0x07:
            self.tex0_2 = value
        elif addr in (0x18, 0x19):  # XYOFFSET: OFX in bytes 0-1, OFY in bytes 4-5 (1/16 pixel)
            self.xyoffset = (_signed16(value & 0xFFFF), _signed16((value >> 32) & 0xFFFF))
            self.xyoffset_seen = True
        elif addr == 0x42:
            self.alpha = value
        elif addr == 0x47:
            self.test = value

    def vertex(self, tag: Tag, qword: bytes, kind: str) -> None:
        """One vertex position qword (XYZ2: 32-bit Z; XYZF2: 24-bit Z and a fog byte)."""
        x = struct.unpack_from('<H', qword, 0)[0]
        y = struct.unpack_from('<H', qword, 4)[0]
        z_word = struct.unpack_from('<I', qword, 8)[0]
        z = z_word if kind == XYZ2 else (z_word >> 4) & 0xFFFFFF
        tag.vertices += 1
        # The GS works with 16-bit modular coordinates: the position is (X - OFX) / 16, both 16-bit,
        # and the difference is what is signed. The game's XYOFFSET puts the display window at
        # [0, 640] x [0, 448] (measured: with the XYOFFSET the dumps write before the frame, the
        # memory-card dialog's vertices fall inside exactly that window).
        dx = (x - self.xyoffset[0]) & 0xFFFF
        dy = (y - self.xyoffset[1]) & 0xFFFF
        px = (_signed16(dx)) / 16.0
        py = (_signed16(dy)) / 16.0
        if tag.xy_min is None:
            tag.xy_min = (px, py)
            tag.xy_max = (px, py)
            tag.z_min = tag.z_max = z
        else:
            tag.xy_min = (min(tag.xy_min[0], px), min(tag.xy_min[1], py))
            tag.xy_max = (max(tag.xy_max[0], px), max(tag.xy_max[1], py))
            tag.z_min = min(tag.z_min, z)
            tag.z_max = max(tag.z_max, z)

    def skip_vertex(self, tag: Tag) -> None:
        """XYZF3 / XYZ3: a vertex marker the GS skips (it does not draw it, no position to record)."""
        tag.vertices += 1

    def parse(self, payload: bytes, transfer: int, path: int) -> list[Tag]:
        """Every tag of one transfer payload."""
        tags: list[Tag] = []
        pos = 0
        while pos + 16 <= len(payload):
            lo, hi = struct.unpack_from('<QQ', payload, pos)
            nloop = lo & NLOOP_MASK
            eop = (lo >> EOP_BIT) & 1
            pre = (lo >> PRE_BIT) & 1
            prim = (lo >> PRIM_SHIFT) & 0x7FF
            flg = (lo >> FLG_SHIFT) & 3
            nreg = ((lo >> NREG_SHIFT) & 0xF) or 16
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            tag = Tag(transfer=transfer, path=path, nloop=nloop, eop=eop, pre=pre,
                      prim=(prim if pre else self.prim), flg=flg, nreg=nreg, regs=regs, qwords=0)
            pos += 16
            if pre and flg == FLG_PACKED:
                self.prim = prim
                self.context = 2 if (prim >> 9) & 1 else 1
            if flg == FLG_DISABLED:
                tag.tex0 = tex0_fields(self.current_tex0())
                tags.append(tag)
                if eop:
                    break
                continue
            count = nloop * nreg if flg != FLG_IMAGE else nloop
            if flg == FLG_IMAGE:
                # IMAGE: NLOOP qwords of raw pixels (NREG is ignored; pcsx2/GS/GSState.cpp GIF_FLG_IMAGE).
                tag.image_qwords = count
                need = count * 16
                if pos + need > len(payload):
                    raise GifStatsError(f'image tag {len(tags)} of transfer {transfer} wants {need} '
                                        f'bytes, {len(payload) - pos} left')
                pos += need
            else:
                tag.qwords = count
                need = count * 16
                if pos + need > len(payload):
                    raise GifStatsError(f'tag {len(tags)} of transfer {transfer} wants {need} bytes, '
                                        f'{len(payload) - pos} left')
                self.relist_vertex_index = 0
                for i in range(count):
                    qword = payload[pos + i * 16:pos + i * 16 + 16]
                    reg = regs[i % nreg]
                    if flg == FLG_PACKED:
                        self._packed_qword(tag, reg, qword)
                    else:
                        self._relist_qword(tag, reg, qword)
                pos += need
            tag.tex0 = tex0_fields(self.current_tex0())
            tags.append(tag)
            if eop:
                break
        return tags

    def _packed_qword(self, tag: Tag, reg: int, qword: bytes) -> None:
        """PACKED: the register list names one register per qword; A+D addresses one inline."""
        if reg == 0x0E:
            addr = qword[8] & 0x7F
            if addr == 0x04:
                self.vertex(tag, qword, XYZF2)
            elif addr == 0x05:
                self.vertex(tag, qword, XYZ2)
            elif addr in (0x0C, 0x0D):
                self.skip_vertex(tag)
            else:
                self.write_register(addr, qword)
        elif reg == 0x04:
            self.vertex(tag, qword, XYZF2)
        elif reg == 0x05:
            self.vertex(tag, qword, XYZ2)
        elif reg in (0x02, 0x03):  # STQ / UV: this vertex is textured
            tag.st = True
        else:
            self.write_register(reg, qword)

    def _relist_qword(self, tag: Tag, reg: int, qword: bytes) -> None:
        """REGLIST: one qword per register id, in the order of the list."""
        if reg in REGLIST_VERTEX3:
            # The three-qword format STQ, RGBAQ, XYZ: the position is the third qword of each vertex.
            index = self.relist_vertex_index % 3
            self.relist_vertex_index += 1
            if index == 0:
                tag.st = True
            elif index == 2:
                self.vertex(tag, qword, REGLIST_VERTEX3[reg])
        elif reg == 0x04:  # A+D
            addr = qword[8] & 0x7F
            if addr == 0x04:
                self.vertex(tag, qword, XYZF2)
            elif addr == 0x05:
                self.vertex(tag, qword, XYZ2)
            elif addr in (0x0C, 0x0D):
                self.skip_vertex(tag)
            else:
                self.write_register(addr, qword)
        elif reg == 0x05:
            self.vertex(tag, qword, XYZ2)
        elif reg in (0x02, 0x03):
            tag.st = True
        else:
            self.write_register(reg, qword)


# ---- containers -----------------------------------------------------------------------------------

def read_bytes(path: Path) -> bytes:
    """The raw bytes of a .gs / .gs.zst dump or of any file (same helper as dump_regs.py)."""
    if path.suffix == '.zst':
        tool = shutil.which('zstd')
        if not tool:
            raise GifStatsError('the zstd command line tool is needed to read .zst dumps')
        result = subprocess.run([tool, '-dc', str(path)], capture_output=True)
        if result.returncode != 0:
            raise GifStatsError(f'zstd failed: {result.stderr.decode(errors="replace").strip()}')
        return result.stdout
    return path.read_bytes()


def _dump_start(data: bytes) -> int:
    """The offset of the first packet of a PCSX2 dump (after the header, state and priv registers)."""
    if len(data) < 16 or struct.unpack_from('<I', data, 0)[0] != 0xFFFFFFFF:
        raise GifStatsError('not a GS dump with the new header (no 0xFFFFFFFF at the start)')
    header_size = struct.unpack_from('<I', data, 4)[0]
    state_size = struct.unpack_from('<I', data, 12)[0]
    return 8 + header_size + state_size + 0x2000


def dump_frames(data: bytes) -> list[list[tuple[int, int, bytes]]]:
    """The transfer packets of a dump, split into frames at the vsync packets."""
    pos = _dump_start(data)
    frames: list[list[tuple[int, int, bytes]]] = []
    current: list[tuple[int, int, bytes]] = []
    index = 0
    while pos < len(data):
        packet = data[pos]
        pos += 1
        if packet == 0:
            if pos + 5 > len(data):
                break
            path = data[pos]
            size = struct.unpack_from('<I', data, pos + 1)[0]
            payload = data[pos + 5:pos + 5 + size]
            if len(payload) < size:
                raise GifStatsError(f'truncated transfer {index} ({len(payload)} of {size} bytes)')
            current.append((index, path, payload))
            index += 1
            pos += 5 + size
        elif packet == 1:
            pos += 1
            frames.append(current)
            current = []
        elif packet == 2:
            pos += 4
        elif packet == 3:
            pos += 0x2000
        else:
            raise GifStatsError(f'unknown packet id {packet} at offset {pos - 1}')
    if current:
        frames.append(current)
    return frames


def parse_dump(path: Path, frame: int = -1) -> tuple[list[Tag], int | None, int]:
    """The tags of one frame of a PCSX2 dump. Returns (tags, frame number or None, count of frames)."""
    frames = dump_frames(read_bytes(path))
    if not frames:
        raise GifStatsError('the dump has no vsync interval')
    index = frame - 1 if frame > 0 else len(frames) - 1
    if not 0 <= index < len(frames):
        raise GifStatsError(f'frame {frame} out of range (the dump has {len(frames)} vsync intervals)')
    parser = GifParser()
    tags: list[Tag] = []
    for number, path_index, payload in frames[index]:
        tags.extend(parser.parse(payload, number, path_index))
    return tags, (index + 1), len(frames)


def parse_capture(path: Path) -> tuple[list[Tag], int | None, int]:
    """The tags of the port's own capture. Returns (tags, frame number or None, count of frames)."""
    data = path.read_bytes()
    if not data.startswith(PORT_MAGIC):
        raise GifStatsError('not a port capture (no DONGS1 magic)')
    frame = struct.unpack_from('<I', data, len(PORT_MAGIC))[0]
    pos = len(PORT_MAGIC) + 4
    parser = GifParser()
    tags: list[Tag] = []
    index = 0
    while pos < len(data):
        if pos + 5 > len(data):
            raise GifStatsError('truncated record header')
        path_index = data[pos]
        size = struct.unpack_from('<I', data, pos + 1)[0]
        payload = data[pos + 5:pos + 5 + size]
        if len(payload) < size:
            raise GifStatsError(f'truncated record {index} ({len(payload)} of {size} bytes)')
        tags.extend(parser.parse(payload, index, path_index))
        index += 1
        pos += 5 + size
    return tags, frame, 1


# ---- the summary ----------------------------------------------------------------------------------

def tag_line(tag: Tag) -> str:
    parts = [f'xfer{tag.transfer} path={tag.path} nloop={tag.nloop} eop={tag.eop} pre={tag.pre}',
             f'prim={tag.prim_class} tme={tag.tme} fge={tag.fge} abe={tag.abe}',
             f'flg={tag.flg} nreg={tag.nreg}', f'regs={",".join(f"{r:02x}" for r in tag.regs)}',
             f'v={tag.vertices}']
    if tag.image_qwords:
        parts.append(f'image_qw={tag.image_qwords}')
    if tag.xy_min is not None:
        parts.append(f'xy={tag.xy_min[0]:.1f},{tag.xy_min[1]:.1f}..{tag.xy_max[0]:.1f},{tag.xy_max[1]:.1f}px')
    if tag.z_min is not None:
        parts.append(f'z={tag.z_min:#x}..{tag.z_max:#x}')
    if tag.tme and tag.tex0 is not None:
        t = tag.tex0
        parts.append(f'tex(tbp={t["tbp0_bytes"]:#x} tbw={t["tbw"]} psm={t["psm"]} {t["tw"]}x{t["th"]} '
                     f'tcc={t["tcc"]} tfx={t["tfx"]} cbp={t["cbp_bytes"]:#x})')
    return ' '.join(parts)


def summarise(tags: list[Tag], source: str) -> str:
    out = [source]
    out.append(f'tags={len(tags)} vertices={sum(t.vertices for t in tags)} '
               f'qwords={sum(t.qwords for t in tags)} image_qwords={sum(t.image_qwords for t in tags)}')
    by_path: dict[int, Counter] = defaultdict(Counter)
    for t in tags:
        by_path[t.path]['tags'] += 1
        by_path[t.path]['vertices'] += t.vertices
        by_path[t.path]['qwords'] += t.qwords
    for path_index in sorted(by_path):
        c = by_path[path_index]
        out.append(f'  path {path_index}: tags={c["tags"]} vertices={c["vertices"]} qwords={c["qwords"]}')
    classes: Counter = Counter()
    textured: Counter = Counter()
    for t in tags:
        if t.vertices:
            classes[t.prim_class] += t.vertices
        if t.tme and t.vertices:
            textured[(t.tex0 or {}).get('tbp0_bytes', 0)] += t.vertices
    if classes:
        out.append('  vertices per primitive: ' + ', '.join(f'{k}={v}' for k, v in classes.most_common()))
    if textured:
        out.append('  textured vertices per TBP0: ' +
                   ', '.join(f'0x{k:x}={v}' for k, v in textured.most_common(12)))
    xyz = [t for t in tags if t.xy_min is not None]
    if xyz:
        out.append('  positions of every vertex: x=[%.1f,%.1f] y=[%.1f,%.1f] px' % (
            min(t.xy_min[0] for t in xyz), max(t.xy_max[0] for t in xyz),
            min(t.xy_min[1] for t in xyz), max(t.xy_max[1] for t in xyz)))
        inside = sum(1 for t in xyz if t.xy_min[0] >= -8 and t.xy_max[0] <= 648 and
                     t.xy_min[1] >= -8 and t.xy_max[1] <= 456)
        out.append(f'  tags with every vertex inside the 640x448 window: {inside} of {len(xyz)}')
    return '\n'.join(out)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('file', type=Path)
    parser.add_argument('--frame', type=int, default=-1,
                        help='which vsync interval of a dump (1-based; default the last)')
    parser.add_argument('--tags', action='store_true', help='print every tag, not only the summary')
    parser.add_argument('--json', type=Path, default=None, help='also write the tags as JSON')
    args = parser.parse_args(argv)

    try:
        raw = read_bytes(args.file)
        if raw.startswith(PORT_MAGIC):
            tags, frame, frames = parse_capture(args.file)
            source = f'# {args.file} (port capture, frame {frame}, path = the runtime\'s gif_transfer path)'
        else:
            tags, frame, frames = parse_dump(args.file, args.frame)
            source = (f'# {args.file} (PCSX2 dump, frame {frame} of {frames}; the dump path index is not '
                      f'the game\'s GIF path: docs/knowledge/15-gs-dumps.md)')
    except (GifStatsError, OSError) as error:
        print(f'gif_stats: {error}', file=sys.stderr)
        return 1

    print(summarise(tags, source))
    if args.tags:
        for t in tags:
            print('  ' + tag_line(t))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'source': source, 'frame': frame,
                                         'tags': [t.to_dict() for t in tags]}, indent=1))
        print(f'# json: {args.json}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
