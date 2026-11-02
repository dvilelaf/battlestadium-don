"""Reference model of the R5900 multimedia (MMI) instructions, written from third_party/pcsx2/pcsx2/MMI.cpp.

A general register is read as one 128-bit number (low half = UD[0]); the lane views follow the reference's
unions: UC/SC bytes, US/SS halfwords, UL/SL words, UD/SD doublewords, lane 0 = the least significant. HI and LO are
128-bit too: HI.UD[0] is the state's HI and HI.UD[1] is HI1 (same for LO, LO1). The model functions are named
after the reference's functions and each cites its line in MMI.cpp (lines of the reference checked out in
third_party/pcsx2; the file is not versioned here).

`exec_mmi(m, funct, sub, rd, rs, rt, sa)` runs one instruction of opcode 0x1C on the ref.Machine `m`: `funct` is
the function field, `sub` the sa field that picks the instruction inside MMI0..MMI3 (funct 0x08, 0x28, 0x09, 0x29),
and for the shifts and PMFHL/PMTHL `sa` is the operand. It returns False if the (funct, sub) is not modelled.
"""
from __future__ import annotations

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
HI, LO, HI1, LO1, SA = 32, 33, 34, 35, 36   # indexes in the state, as in ref.py


def lanes(x: int, w: int) -> list[int]:
    return [(x >> (w * i)) & ((1 << w) - 1) for i in range(128 // w)]


def pack(vals: list[int], w: int) -> int:
    mask = (1 << w) - 1
    out = 0
    for i, v in enumerate(vals):
        out |= (v & mask) << (w * i)
    return out


def sg(x: int, w: int) -> int:
    x &= (1 << w) - 1
    return x - (1 << w) if x >> (w - 1) else x


def _cdiv(a: int, b: int) -> int:
    """C integer division, truncated toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def _crem(a: int, b: int) -> int:
    return a - _cdiv(a, b) * b


def get(m, r: int) -> int:
    return m.lo[r] | (m.hi[r] << 64)


def put(m, r: int, v: int) -> None:
    if r:
        m.lo[r], m.hi[r] = v & M64, (v >> 64) & M64


def get_hi(m) -> int:
    return m.lo[HI] | (m.lo[HI1] << 64)


def get_lo(m) -> int:
    return m.lo[LO] | (m.lo[LO1] << 64)


def put_hi(m, v: int) -> None:
    m.lo[HI], m.lo[HI1] = v & M64, (v >> 64) & M64


def put_lo(m, v: int) -> None:
    m.lo[LO], m.lo[LO1] = v & M64, (v >> 64) & M64


def _sd(l32: list[int], dd: int, v: int) -> None:
    """X.SD[dd] = v on the word lanes of HI or LO."""
    l32[2 * dd], l32[2 * dd + 1] = v & M32, (v >> 32) & M32


def _clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


# ---- lane-wise two-operand forms: f(a, b) on the unsigned lanes of Rs and Rt --------------------------------------
def _binary(w: int, f):
    def go(s: int, t: int) -> int:
        return pack([f(a, b) for a, b in zip(lanes(s, w), lanes(t, w))], w)
    return go


def _add(w): return _binary(w, lambda a, b: a + b)                                      # PADDW/H/B :312, :351, :386
def _sub(w): return _binary(w, lambda a, b: a - b)                                      # PSUBW/H/B
def _cgt(w): return _binary(w, lambda a, b: -1 if sg(a, w) > sg(b, w) else 0)           # PCGTW/H/B
def _max(w): return _binary(w, lambda a, b: a if sg(a, w) > sg(b, w) else b)            # PMAXW/H (Rs when greater)
def _min(w): return _binary(w, lambda a, b: a if sg(a, w) < sg(b, w) else b)            # PMINW/H
def _ceq(w): return _binary(w, lambda a, b: -1 if a == b else 0)                        # PCEQW/H/B
def _adds(w): return _binary(w, lambda a, b: _clamp(sg(a, w) + sg(b, w), -(1 << (w - 1)), (1 << (w - 1)) - 1))   # PADDSW/H/B
def _subs(w): return _binary(w, lambda a, b: _clamp(sg(a, w) - sg(b, w), -(1 << (w - 1)), (1 << (w - 1)) - 1))   # PSUBSW/H/B
def _addu(w): return _binary(w, lambda a, b: min(a + b, (1 << w) - 1))                  # PADDUW/H/B
def _subu(w): return _binary(w, lambda a, b: max(a - b, 0))                             # PSUBUW/H/B


def _pextl(w):                                                                          # PEXTLW/H/B: low halves interleaved
    def go(s, t):
        sl, tl = lanes(s, w), lanes(t, w)
        h = len(sl) // 2
        return pack([x for i in range(h) for x in (tl[i], sl[i])], w)
    return go


def _pextu(w):                                                                          # PEXTUW/H/B: high halves interleaved
    def go(s, t):
        sl, tl = lanes(s, w), lanes(t, w)
        h = len(sl) // 2
        return pack([x for i in range(h) for x in (tl[h + i], sl[h + i])], w)
    return go


def _ppac(w):                                                                           # PPACW/H/B: even lanes of Rt, then of Rs
    def go(s, t):
        return pack(lanes(t, w)[0::2] + lanes(s, w)[0::2], w)
    return go


def _unary(w, f):
    return lambda s, t: pack([f(x) for x in lanes(t, w)], w)


def _pabs(w):                                                                           # PABSW :593, PABSH :627
    mn = 1 << (w - 1)
    return _unary(w, lambda x: mn - 1 if x == mn else (-sg(x, w) if sg(x, w) < 0 else x))


def _perm(w, order):
    return lambda s, t: pack([lanes(t, w)[i] for i in order], w)


def _pext5(s, t):                                                                       # PEXT5 :538
    return pack([((x & 0x1F) << 3) | ((x & 0x3E0) << 6) | ((x & 0x7C00) << 9) | ((x & 0x8000) << 16)
                 for x in lanes(t, 32)], 32)


def _ppac5(s, t):                                                                       # PPAC5 :556
    return pack([((x >> 3) & 0x1F) | ((x >> 6) & 0x3E0) | ((x >> 9) & 0x7C00) | ((x >> 16) & 0x8000)
                 for x in lanes(t, 32)], 32)


def _padsbh(s, t):                                                                      # PADSBH :671: halfwords 0-3 subtract, 4-7 add
    sl, tl = lanes(s, 16), lanes(t, 16)
    return pack([sl[i] - tl[i] if i < 4 else sl[i] + tl[i] for i in range(8)], 16)


def _pinth(s, t):                                                                       # PINTH :1103
    sl, tl = lanes(s, 16), lanes(t, 16)
    return pack([tl[0], sl[4], tl[1], sl[5], tl[2], sl[6], tl[3], sl[7]], 16)


def _pinteh(s, t):                                                                      # PINTEH :1478
    sl, tl = lanes(s, 16), lanes(t, 16)
    return pack([tl[0], sl[0], tl[2], sl[2], tl[4], sl[4], tl[6], sl[6]], 16)


def _pcpyld(s, t):                                                                      # PCPYLD :1173: UD[1] = Rs.UD[0], UD[0] = Rt.UD[0]
    return (t & M64) | ((s & M64) << 64)


def _pcpyud(s, t):                                                                      # PCPYUD :1537: UD[0] = Rs.UD[1], UD[1] = Rt.UD[1]
    return (s >> 64) | ((t >> 64) << 64)


def _psllvw(s, t):                                                                      # PSLLVW :971, PSRLVW :979, PSRAVW :1432
    return _shiftv(s, t, lambda x, n: x << n)


def _psrlvw(s, t):
    return _shiftv(s, t, lambda x, n: x >> n)


def _psravw(s, t):
    return _shiftv(s, t, lambda x, n: sg(x, 32) >> n)


def _shiftv(s, t, f):
    sl, tl = lanes(s, 32), lanes(t, 32)
    out = 0
    for dd, i in ((0, 0), (1, 2)):
        v = f(tl[i], sl[i] & 0x1F) & M32
        out |= (v | (M32 << 32 if v >> 31 else 0)) << (64 * dd)       # (s64)(s32)(...)
    return out


def _qfsrv(m, s: int, t: int) -> int:                                                  # QFSRV :869 (the shift is in bytes)
    sa_amt = (m.lo[SA] << 3) & M32
    s0, s1, t0, t1 = s & M64, s >> 64, t & M64, t >> 64
    if sa_amt == 0:
        return t
    if sa_amt < 64:
        d0 = (t0 >> sa_amt) | ((t1 << (64 - sa_amt)) & M64)
        d1 = (t1 >> sa_amt) | ((s0 << (64 - sa_amt)) & M64)
    else:
        n = sa_amt - 64
        d0, d1 = t1 >> n, s0 >> n
        if sa_amt != 64:
            d0 |= (s0 << (128 - sa_amt)) & M64
            d1 |= (s1 << (128 - sa_amt)) & M64
    return d0 | (d1 << 64)


# ---- the table of the pure (no HI/LO) forms ------------------------------------------------------------------------
PURE = {
    (0x08, 0x00): _add(32), (0x08, 0x01): _sub(32), (0x08, 0x02): _cgt(32), (0x08, 0x03): _max(32),
    (0x08, 0x04): _add(16), (0x08, 0x05): _sub(16), (0x08, 0x06): _cgt(16), (0x08, 0x07): _max(16),
    (0x08, 0x08): _add(8), (0x08, 0x09): _sub(8), (0x08, 0x0A): _cgt(8),
    (0x08, 0x10): _adds(32), (0x08, 0x11): _subs(32), (0x08, 0x12): _pextl(32), (0x08, 0x13): _ppac(32),
    (0x08, 0x14): _adds(16), (0x08, 0x15): _subs(16), (0x08, 0x16): _pextl(16), (0x08, 0x17): _ppac(16),
    (0x08, 0x18): _adds(8), (0x08, 0x19): _subs(8), (0x08, 0x1A): _pextl(8), (0x08, 0x1B): _ppac(8),
    (0x08, 0x1E): _pext5, (0x08, 0x1F): _ppac5,
    (0x28, 0x01): _pabs(32), (0x28, 0x02): _ceq(32), (0x28, 0x03): _min(32), (0x28, 0x04): _padsbh,
    (0x28, 0x05): _pabs(16), (0x28, 0x06): _ceq(16), (0x28, 0x07): _min(16), (0x28, 0x0A): _ceq(8),
    (0x28, 0x10): _addu(32), (0x28, 0x11): _subu(32), (0x28, 0x12): _pextu(32),
    (0x28, 0x14): _addu(16), (0x28, 0x15): _subu(16), (0x28, 0x16): _pextu(16),
    (0x28, 0x18): _addu(8), (0x28, 0x19): _subu(8), (0x28, 0x1A): _pextu(8),
    (0x09, 0x02): _psllvw, (0x09, 0x03): _psrlvw, (0x29, 0x03): _psravw,
    (0x09, 0x0A): _pinth, (0x09, 0x0E): _pcpyld,
    (0x09, 0x12): lambda s, t: s & t, (0x09, 0x13): lambda s, t: s ^ t,           # PAND :1257, PXOR :1263
    (0x29, 0x12): lambda s, t: s | t, (0x29, 0x13): lambda s, t: ~(s | t) & ((1 << 128) - 1),   # POR :1546, PNOR :1553
    (0x09, 0x1A): _perm(16, [2, 1, 0, 3, 6, 5, 4, 7]),                              # PEXEH :1331
    (0x09, 0x1B): _perm(16, [3, 2, 1, 0, 7, 6, 5, 4]),                              # PREVH :1347
    (0x09, 0x1E): _perm(32, [2, 1, 0, 3]),                                          # PEXEW :1428
    (0x09, 0x1F): _perm(32, [1, 2, 0, 3]),                                          # PROT3W :1438
    (0x29, 0x0A): _pinteh, (0x29, 0x0E): _pcpyud,
    (0x29, 0x1A): _perm(16, [0, 2, 1, 3, 4, 6, 5, 7]),                              # PEXCH :1565
    (0x29, 0x1B): _perm(16, [0, 0, 0, 0, 4, 4, 4, 4]),                              # PCPYH :1579
    (0x29, 0x1E): _perm(32, [0, 2, 1, 3]),                                          # PEXCW :1597
}
SHIFTS = {                                                                          # PSLLH :161 ... PSRAW :234 (rd, rt, sa)
    0x34: lambda t, sa: pack([x << (sa & 0xF) for x in lanes(t, 16)], 16),
    0x36: lambda t, sa: pack([x >> (sa & 0xF) for x in lanes(t, 16)], 16),
    0x37: lambda t, sa: pack([sg(x, 16) >> (sa & 0xF) for x in lanes(t, 16)], 16),
    0x3C: lambda t, sa: pack([x << sa for x in lanes(t, 32)], 32),
    0x3E: lambda t, sa: pack([x >> sa for x in lanes(t, 32)], 32),
    0x3F: lambda t, sa: pack([sg(x, 32) >> sa for x in lanes(t, 32)], 32),
}


def _write_rd_pairs(m, rd: int, lo_l: list[int], hi_l: list[int]) -> None:
    if rd:                                                  # Rd = LO.UL[0], HI.UL[0], LO.UL[2], HI.UL[2]
        put(m, rd, pack([lo_l[0], hi_l[0], lo_l[2], hi_l[2]], 32))


def _store(m, lo_l: list[int], hi_l: list[int]) -> None:
    put_lo(m, pack(lo_l, 32))
    put_hi(m, pack(hi_l, 32))


def _mul16(m, rd, s, t, kind):                              # PMADDH :1190, PMSUBH :1289, PMULTH :1371, PHMADH :1224, PHMSBH :1322
    sl, tl = lanes(s, 16), lanes(t, 16)
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    if kind in ("pmaddh", "pmsubh", "pmulth"):
        targets = [(lo_l, 0), (lo_l, 1), (hi_l, 0), (hi_l, 1), (lo_l, 2), (lo_l, 3), (hi_l, 2), (hi_l, 3)]
        for i, (arr, k) in enumerate(targets):
            p = sg(sl[i], 16) * sg(tl[i], 16)
            arr[k] = ((arr[k] + p) if kind == "pmaddh" else (arr[k] - p) if kind == "pmsubh" else p) & M32
    else:
        for arr, dd, n in ((lo_l, 0, 0), (hi_l, 0, 2), (lo_l, 2, 4), (hi_l, 2, 6)):
            first = sg(sl[n + 1], 16) * sg(tl[n + 1], 16)
            second = sg(sl[n], 16) * sg(tl[n], 16)
            if kind == "phmadh":
                arr[dd], arr[dd + 1] = (first + second) & M32, first & M32
            else:
                arr[dd], arr[dd + 1] = (first - second) & M32, ~first & M32      # "undocumented behaviour" of the reference
    _store(m, lo_l, hi_l)
    _write_rd_pairs(m, rd, lo_l, hi_l)


def _mulw(m, rd, s, t, kind):                               # PMADDW :1056, PMSUBW :1021, PMULTW :1123, PMADDUW :1403, PMULTUW :1491
    sl, tl = lanes(s, 32), lanes(t, 32)
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    for dd, ss in ((0, 0), (1, 2)):
        if kind in ("pmaddw", "pmsubw"):
            temp = sg(sl[ss], 32) * sg(tl[ss], 32)
            if kind == "pmaddw":
                temp2 = sg(temp + (sg(hi_l[ss], 32) << 32), 64)
                if ss == 0 and ((tl[ss] & 0x7FFFFFFF) in (0, 0x7FFFFFFF)) and sg(sl[ss], 32) != sg(tl[ss], 32):
                    temp2 = sg(temp2 + 0x70000000, 64)                 # "PlayStation 2 division voodoo"
                new_lo = sg(sg(temp, 32) + sg(lo_l[ss], 32), 32)
            else:
                temp2 = sg((sg(hi_l[ss], 32) << 32) - temp, 64)
                new_lo = sg(sg(lo_l[ss], 32) - sg(temp, 32), 32)
            temp2 = sg(_cdiv(temp2, 4294967295), 32)                   # "off by 1" division of the reference
            _sd(lo_l, dd, new_lo & M64)
            _sd(hi_l, dd, temp2 & M64)
            if rd:
                pass
        elif kind == "pmultw":
            temp = sg(sl[ss], 32) * sg(tl[ss], 32)
            _sd(lo_l, dd, sg(temp, 32) & M64)
            _sd(hi_l, dd, sg(temp >> 32, 32) & M64)
        elif kind == "pmultuw":
            tu = sl[ss] * tl[ss]
            _sd(lo_l, dd, sg(tu, 32) & M64)
            _sd(hi_l, dd, sg(tu >> 32, 32) & M64)
        else:                                                           # pmadduw
            tu = ((lo_l[ss] | (hi_l[ss] << 32)) + sl[ss] * tl[ss]) & M64
            _sd(lo_l, dd, sg(tu, 32) & M64)
            _sd(hi_l, dd, sg(tu >> 32, 32) & M64)
    _store(m, lo_l, hi_l)
    if rd:
        if kind in ("pmaddw", "pmsubw"):
            _write_rd_pairs(m, rd, lo_l, hi_l)
        elif kind == "pmultw":
            put(m, rd, pack([sg(sl[0], 32) * sg(tl[0], 32), sg(sl[2], 32) * sg(tl[2], 32)], 64))
        elif kind == "pmultuw":
            put(m, rd, pack([sl[0] * tl[0], sl[2] * tl[2]], 64))
        else:
            put(m, rd, pack([((lanes(get_lo(m), 32)[0] | (lanes(get_hi(m), 32)[0] << 32))),
                             ((lanes(get_lo(m), 32)[2] | (lanes(get_hi(m), 32)[2] << 32)))], 64))


def _pdivw(m, s, t):                                        # PDIVW :1141
    sl, tl = lanes(s, 32), lanes(t, 32)
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    for dd, ss in ((0, 0), (1, 2)):
        if sl[ss] == 0x80000000 and tl[ss] == 0xFFFFFFFF:
            lq, hr = sg(0x80000000, 32), 0
        elif sg(tl[ss], 32) != 0:
            lq, hr = _cdiv(sg(sl[ss], 32), sg(tl[ss], 32)), _crem(sg(sl[ss], 32), sg(tl[ss], 32))
        else:
            lq, hr = (1 if sg(sl[ss], 32) < 0 else -1), sg(sl[ss], 32)
        _sd(lo_l, dd, lq & M64)
        _sd(hi_l, dd, hr & M64)
    _store(m, lo_l, hi_l)


def _pdivuw(m, s, t):                                       # PDIVUW :1517
    sl, tl = lanes(s, 32), lanes(t, 32)
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    for dd, ss in ((0, 0), (1, 2)):
        if tl[ss] != 0:
            lq, hr = sg(sl[ss] // tl[ss], 32), sg(sl[ss] % tl[ss], 32)
        else:
            lq, hr = -1, sg(sl[ss], 32)
        _sd(lo_l, dd, lq & M64)
        _sd(hi_l, dd, hr & M64)
    _store(m, lo_l, hi_l)


def _pdivbw(m, s, t):                                       # PDIVBW :1389 (divisor: the low halfword of Rt)
    sl, tl16 = lanes(s, 32), lanes(t, 16)[0]
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    for n in range(4):
        if sl[n] == 0x80000000 and tl16 == 0xFFFF:
            lo_l[n], hi_l[n] = 0x80000000, 0
        elif tl16 != 0:
            lo_l[n] = _cdiv(sg(sl[n], 32), sg(tl16, 16)) & M32
            hi_l[n] = _crem(sg(sl[n], 32), sg(tl16, 16)) & M32
        else:
            lo_l[n] = (1 if sg(sl[n], 32) < 0 else -1) & M32
            hi_l[n] = sl[n]
    _store(m, lo_l, hi_l)


def _pmfhl(m, rd, fmt):                                     # PMFHL :108
    if not rd:
        return
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    lo16, hi16 = lanes(get_lo(m), 16), lanes(get_hi(m), 16)
    if fmt == 0:
        put(m, rd, pack([lo_l[0], hi_l[0], lo_l[2], hi_l[2]], 32))
    elif fmt == 1:
        put(m, rd, pack([lo_l[1], hi_l[1], lo_l[3], hi_l[3]], 32))
    elif fmt == 2:
        halves = []
        for i in (0, 2):
            t = sg(lo_l[i] | (hi_l[i] << 32), 64)
            halves.append(0x7FFFFFFF if t >= 0x7FFFFFFF else (0xFFFFFFFF80000000 if t <= -0x80000000 else sg(lo_l[i], 32) & M64))
        put(m, rd, pack(halves, 64))
    elif fmt == 3:
        put(m, rd, pack([lo16[0], lo16[2], hi16[0], hi16[2], lo16[4], lo16[6], hi16[4], hi16[6]], 16))
    elif fmt == 4:
        srcs = [lo_l[0], lo_l[1], hi_l[0], hi_l[1], lo_l[2], lo_l[3], hi_l[2], hi_l[3]]
        put(m, rd, pack([_clamp(sg(x, 32), -0x8000, 0x7FFF) for x in srcs], 16))     # PMFHL_CLAMP: >0x7fff, < -0x8000


def _pmthl(m, rs, fmt):                                     # PMTHL :195
    if fmt != 0:
        return
    s = lanes(get(m, rs), 32)
    lo_l, hi_l = lanes(get_lo(m), 32), lanes(get_hi(m), 32)
    lo_l[0], hi_l[0], lo_l[2], hi_l[2] = s[0], s[1], s[2], s[3]
    _store(m, lo_l, hi_l)


def _plzcw(m, rd, rs):                                      # PLZCW :95 (Common::CountLeadingSignBits(s32) - 1 on words 0 and 1)
    if not rd:
        return
    out = []
    for x in lanes(get(m, rs), 32)[:2]:
        n = sg(x, 32)
        n = ~n if n < 0 else n
        out.append(((32 if n == 0 else 32 - n.bit_length()) - 1) & M32)
    put(m, rd, (get(m, rd) & ~M64 & ((1 << 128) - 1)) | pack(out, 32))


MUL16 = {(0x09, 0x10): "pmaddh", (0x09, 0x14): "pmsubh", (0x09, 0x1C): "pmulth", (0x09, 0x11): "phmadh", (0x09, 0x15): "phmsbh"}
MULW = {(0x09, 0x00): "pmaddw", (0x09, 0x04): "pmsubw", (0x09, 0x0C): "pmultw", (0x29, 0x00): "pmadduw", (0x29, 0x0C): "pmultuw"}


def exec_mmi(m, funct: int, sub: int, rd: int, rs: int, rt: int, sa: int) -> bool:
    s, t = get(m, rs), get(m, rt)
    key = (funct, sub)
    if funct == 0x04:
        _plzcw(m, rd, rs)
    elif funct == 0x30:
        _pmfhl(m, rd, sa)
    elif funct == 0x31:
        _pmthl(m, rs, sa)
    elif funct in SHIFTS:
        if rd:
            put(m, rd, SHIFTS[funct](t, sa))
    elif key == (0x28, 0x1B):
        if rd:
            put(m, rd, _qfsrv(m, s, t))
    elif key in PURE:
        if rd:
            put(m, rd, PURE[key](s, t))
    elif key in MUL16:
        _mul16(m, rd, s, t, MUL16[key])
    elif key in MULW:
        _mulw(m, rd, s, t, MULW[key])
    elif key == (0x09, 0x0D):
        _pdivw(m, s, t)
    elif key == (0x29, 0x0D):
        _pdivuw(m, s, t)
    elif key == (0x09, 0x1D):
        _pdivbw(m, s, t)
    elif key == (0x09, 0x08):                               # PMFHI :1085
        put(m, rd, get_hi(m))
    elif key == (0x09, 0x09):                               # PMFLO :1092
        put(m, rd, get_lo(m))
    elif key == (0x29, 0x08):                               # PMTHI :1421
        put_hi(m, s)
    elif key == (0x29, 0x09):                               # PMTLO :1427
        put_lo(m, s)
    else:
        return False
    return True
