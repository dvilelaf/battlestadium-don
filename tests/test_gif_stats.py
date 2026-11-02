"""Tests of tools/verify/gif_stats.py (T-202 step 1) with synthetic GIF packets.

The packets are built here, so nothing needs the game or a console dump:

* a PACKED tag with the register list STQ, RGBAQ, XYZF2 (the format the console's dumps use) and two
  vertices, to check the vertex count, the primitive class, the texture in use and the bounding box
  after the XYOFFSET that an A+D write sets before it;
* A+D writes (the address is byte 8, the data bytes 0-7: `GC 7.1.1`), to check PRIM, TEX0 and
  XYOFFSET decoding, including the `tbp0 = (value & 0x3fff) * 256` rule of the dumps;
* an IMAGE tag (raw pixels: counted, no vertices) and an EOP tag that ends the parse;
* a truncated payload, which must be reported and not crash;
* the two containers: a synthetic PCSX2 dump (magic, header, state block, privileged registers, one
  transfer and a vsync) and a synthetic port capture (`DONGS1`), both through the command line.
"""
import struct
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import gif_stats  # noqa: E402

XYOFFSET_BYTES = struct.pack('<H', 0x6C00) + b'\0\0' + struct.pack('<H', 0x7200) + b'\0\0'


def ad(addr: int, data: bytes = b'\0' * 8) -> bytes:
    """An A+D qword: the 8 data bytes in front, the register address in byte 8."""
    assert len(data) == 8
    return data + bytes([addr]) + b'\0' * 7


def packed_vertex(x: int, y: int, z: int) -> bytes:
    """STQ (three floats), RGBAQ (a colour) and XYZF2: the register list 02, 01, 04."""
    stq = struct.pack('<fff', 0.5, 0.25, 1.0) + b'\0' * 4
    rgba = struct.pack('<4I', 0x66, 0x6F, 0x73, 0x80)
    xyzf2 = struct.pack('<H', x & 0xFFFF) + b'\0\0' + struct.pack('<H', y & 0xFFFF) + b'\0\0'
    xyzf2 += struct.pack('<I', (z << 4) & 0xFFFFFFFF) + b'\0' * 4
    return stq + rgba + xyzf2


def gif_tag(nloop: int, *, eop: int = 1, pre: int = 1, prim: int = 0x12, flg: int = 0,
            regs=(2, 1, 4)) -> bytes:
    """A 16-byte GIFtag: NLOOP, EOP, PRE, PRIM, FLG, NREG and the register list."""
    nreg = len(regs)
    lo = (nloop & 0x7FFF) | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60)
    hi = 0
    for i, reg in enumerate(regs):
        hi |= (reg & 0xF) << (4 * i)
    return struct.pack('<QQ', lo, hi)


def tex0_value(tbp0_bytes: int, tbw: int, psm: int, tw_log2: int, th_log2: int, tcc: int,
               tfx: int) -> bytes:
    value = (tbp0_bytes // 256) & 0x3FFF
    value |= ((tbw // 64) & 0x3F) << 14
    value |= (psm & 0x3F) << 20
    value |= (tw_log2 & 0xF) << 26
    value |= (th_log2 & 0xF) << 30
    value |= (tcc & 1) << 34
    value |= (tfx & 3) << 35
    return struct.pack('<Q', value)


def parse(payload: bytes, default_offset: int = 32768):
    parser = gif_stats.GifParser(default_xyoffset=default_offset)
    return parser, parser.parse(payload, 0, 1)


def test_a_packed_vertex_tag():
    payload = gif_tag(2)
    payload += packed_vertex(0x6C00 + 160, 0x7200 + 320, 100)   # (10, 20) px with the offset below
    payload += packed_vertex(0x6C00 + 480, 0x7200 + 640, 200)   # (30, 40) px
    parser, tags = parse(payload)

    assert len(tags) == 1
    tag = tags[0]
    assert (tag.nloop, tag.eop, tag.pre, tag.flg, tag.nreg) == (2, 1, 1, 0, 3)
    assert tag.regs == [2, 1, 4]
    assert (tag.vertices, tag.qwords) == (2, 6)
    assert tag.prim_class == 'line_strip'        # 0x12     assert tag.prim_class == 'triangle'          # 0x12: bits 0-2 = 2? no: 0x12 & 7 = 2 -> line_strip 7 = 2: a line strip with TME set
    assert tag.tme == 1 and tag.fge == 0 and tag.abe == 0


def test_positions_use_the_xyoffset_the_stream_sets():
    # First an A+D write of XYOFFSET (0x6C00, 0x7200), then a vertex tag.
    writes = gif_tag(1, eop=0, pre=0, prim=0, regs=(0x0E,))
    writes += ad(0x18, XYOFFSET_BYTES)
    vertices = gif_tag(2)
    vertices += packed_vertex(0x6C00 + 160, 0x7200 + 320, 100)
    vertices += packed_vertex(0x6C00 - 160, 0x7200 - 320, 300)
    parser, tags = parse(writes + vertices)

    assert len(tags) == 2
    assert parser.xyoffset == (0x6C00, 0x7200)
    assert parser.xyoffset_seen
    tag = tags[1]
    assert tag.vertices == 2
    assert tag.xy_min == pytest.approx((-10.0, -20.0))
    assert tag.xy_max == pytest.approx((10.0, 20.0))
    assert (tag.z_min, tag.z_max) == (100, 300)


def test_a_prim_tag_uses_the_prims_class():
    payload = gif_tag(1, prim=0x16)   # 0x16 & 7 = 6 (sprite), TME = 1
    payload += packed_vertex(0x6C00, 0x7200, 0)
    _, tags = parse(payload)
    assert tags[0].prim_class == 'sprite'
    assert tags[0].tme == 1


def test_tex0_is_decoded_from_an_a_d_write():
    writes = gif_tag(1, eop=0, pre=0, prim=0, regs=(0x0E,))
    writes += ad(0x06, tex0_value(0x2BC000, 256, 0, 9, 9, 1, 0))   # 512x512, RGBA, modulate
    vertices = gif_tag(1)
    vertices += packed_vertex(0x6C00, 0x7200, 0)
    _, tags = parse(writes + vertices)

    tex0 = tags[-1].tex0
    assert tex0['tbp0_bytes'] == 0x2BC000
    assert tex0['tbw'] == 256
    assert (tex0['tw'], tex0['th']) == (512, 512)
    assert tex0['tcc'] == 1 and tex0['tfx'] == 0


def test_an_image_tag_is_counted_and_has_no_vertices():
    payload = gif_tag(4, eop=1, pre=0, prim=0, flg=gif_stats.FLG_IMAGE, regs=(4,))
    payload += b'\x11' * (4 * 16)
    _, tags = parse(payload)
    assert len(tags) == 1
    assert tags[0].vertices == 0
    assert tags[0].image_qwords == 4


def test_eop_ends_the_parse():
    payload = gif_tag(1, eop=1)
    payload += packed_vertex(0x6C00, 0x7200, 0)
    payload += gif_tag(1, eop=1)                 # a second tag after EOP: not read
    payload += packed_vertex(0x6C00, 0x7200, 0)
    _, tags = parse(payload)
    assert len(tags) == 1 and tags[0].vertices == 1


def test_a_truncated_payload_is_reported():
    payload = gif_tag(2) + packed_vertex(0x6C00, 0x7200, 0)   # one vertex of two
    with pytest.raises(gif_stats.GifStatsError):
        parse(payload)


def _synthetic_dump(payload: bytes) -> bytes:
    """A minimal PCSX2 dump: magic, header, an empty state block, the priv block, one transfer, vsync."""
    state = b'\0' * 16
    priv = b'\0' * 0x2000
    header = struct.pack('<9I', 9, len(state), 0, 0, 0x12345678, 0, 0, 0, 0)
    dump = struct.pack('<II', 0xFFFFFFFF, len(header)) + header + state + priv
    dump += bytes([0]) + bytes([3]) + struct.pack('<I', len(payload)) + payload
    dump += bytes([1]) + bytes([0])
    return dump


def test_a_dump_container_is_split_into_frames(tmp_path):
    path = tmp_path / 'synthetic.gs'
    payload = gif_tag(1) + packed_vertex(0x6C00 + 16, 0x7200 + 16, 0)
    path.write_bytes(_synthetic_dump(payload))
    tags, frame, frames = gif_stats.parse_dump(path)
    assert frames == 1 and frame == 1
    assert len(tags) == 1 and tags[0].vertices == 1
    assert tags[0].path == 3          # the dump's own index, not the game's path
    # No XYOFFSET write in this synthetic frame: the parser falls back to 0x8000 (documented default),
    # so (0x6c00+16, 0x7200+16) is 319 and 223 pixels left of and above the display window.
    assert tags[0].xy_min == pytest.approx((-319.0, -223.0))


def test_a_capture_container_keeps_the_runtime_path(tmp_path):
    path = tmp_path / 'capture.bin'
    payload = gif_tag(1) + packed_vertex(0x6C00, 0x7200, 0)
    body = bytes([1]) + struct.pack('<I', len(payload)) + payload
    body += bytes([2]) + struct.pack('<I', len(payload)) + payload
    path.write_bytes(gif_stats.PORT_MAGIC + struct.pack('<I', 2700) + body)
    tags, frame, _ = gif_stats.parse_capture(path)
    assert frame == 2700
    assert [t.path for t in tags] == [1, 2]


def test_the_command_line_prints_the_summary(tmp_path):
    path = tmp_path / 'capture.bin'
    payload = gif_tag(2) + packed_vertex(0x6C00 + 16, 0x7200 + 16, 0)
    payload += packed_vertex(0x6C00 + 320, 0x7200 + 160, 0)
    path.write_bytes(gif_stats.PORT_MAGIC + struct.pack('<I', 2700) + bytes([1]) +
                     struct.pack('<I', len(payload)) + payload)
    result = subprocess.run([sys.executable, '-I', str(Path(gif_stats.__file__)), str(path)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert 'port capture, frame 2700' in result.stdout
    assert 'tags=1 vertices=2' in result.stdout
    assert 'path 1:' in result.stdout

    bad = tmp_path / 'bad.bin'
    bad.write_bytes(b'not a capture')
    result = subprocess.run([sys.executable, '-I', str(Path(gif_stats.__file__)), str(bad)],
                            capture_output=True, text=True)
    assert result.returncode == 1
    assert 'gif_stats:' in result.stderr
