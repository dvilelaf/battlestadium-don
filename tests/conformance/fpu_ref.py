"""Reference model of the COP1 (floating-point) instructions, from the reference emulator's interpreter
third_party/pcsx2/pcsx2/FPU.cpp (function and line are cited per instruction).

How the arithmetic is done (no use of the host's floats, so the model does not depend on the interpreter's rounding mode):
every operand is turned into an exact `Fraction`, the operation is done exactly (sums, products, quotients) or, for the
square root, with an integer square root of a scaled integer, and the exact value is truncated toward zero to 24
significant bits (`encode`). That is what the console does and what the host does under the game's control-register
value 0xFFC0 (round toward zero, flush-to-zero, denormals-are-zero; src/host/fp_env.cpp). A result below the smallest
normal number is flushed to a zero with the sign of the exact result; one that would overflow is the largest finite
number, because truncation never rounds up to infinity. The interpreter's own checks (FPU.cpp:65-85, checkOverflow and
checkUnderflow) therefore never fire in that mode and only clear their flags; this is what the model does.

The state: 32 fprs at index F0.., the accumulator at ACC and fcr31 at FCR (each an unsigned 32-bit number in the low
half of the pair, like HI/LO/SA in ref.py).
"""
from __future__ import annotations

from fractions import Fraction
from math import isqrt

F0, ACC, FCR = 37, 69, 70
M32 = 0xFFFFFFFF
# FCR31 flags, FPU.cpp:46-54
C, I, D, O, U, SI, SD, SO, SU = 0x800000, 0x20000, 0x10000, 0x8000, 0x4000, 0x40, 0x20, 0x10, 0x8
FMAX = 0x7F7FFFFF

# Which reading of the reference to use for the operations whose interpreter and recompiler differ (set by the harness):
#   0  the interpreter (FPU.cpp) under the game's mode: truncation
#   1  the recompiler (x86/iFPU.cpp) for div, sqrt, rsqrt: FPUDivFPCR defaults to round-to-nearest (Pcsx2Config.cpp:601)
#   2  rsqrt's operands in float arithmetic instead of double (which overload `sqrt` resolves to is not known)
READING = 0


def sext32(x: int) -> int:
    x &= M32
    return (x | ~M32) & 0xFFFFFFFFFFFFFFFF if x >> 31 else x


def clamp(x: int) -> int:
    """fpuDouble (FPU.cpp:140): exponent 0 is a zero of the same sign, exponent 255 is the largest finite number."""
    e = (x >> 23) & 0xFF
    if e == 0:
        return x & 0x80000000
    if e == 0xFF:
        return (x & 0x80000000) | FMAX
    return x


def val(x: int) -> Fraction:
    x = clamp(x)
    m = x & 0x7FFFFF
    e = (x >> 23) & 0xFF
    if e == 0:
        return Fraction(0)
    v = Fraction(0x800000 | m) * (Fraction(2) ** (e - 150))
    return -v if x >> 31 else v


def _ilog2(f: Fraction) -> int:
    e = f.numerator.bit_length() - f.denominator.bit_length()
    return e if f >= Fraction(2) ** e else e - 1


def encode(x: Fraction, zero_sign: int = 0, nearest: bool = False) -> int:
    """x (exact) to the bits of a single, truncated toward zero (or, for the alternative reading, to nearest-even)."""
    if x == 0:
        return zero_sign << 31
    s = 1 if x < 0 else 0
    m = -x if s else x
    e = _ilog2(m)
    # significand scaled to 24 bits: m * 2^(23-e)
    sc = m * (Fraction(2) ** (23 - e))
    q = sc.numerator // sc.denominator
    if nearest:
        rem = sc - q
        if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and q & 1):
            q += 1
            if q == 1 << 24:
                q >>= 1
                e += 1
    if e > 127:
        return (s << 31) | FMAX             # truncation never reaches infinity; for the nearest reading checkOverflow clamps
    if e < -126:
        return s << 31                      # flushed
    return (s << 31) | ((e + 127) << 23) | (q & 0x7FFFFF)


def _sqrt_bits(m: Fraction, nearest: bool = False) -> int:
    """sqrt(m), m > 0 exact, truncated (or nearest) to 24 bits."""
    # scale: floor(sqrt(m * 4^k)) with k large enough to keep > 26 bits
    k = 30 - (_ilog2(m) // 2)
    n = m * (Fraction(4) ** k)
    ni = n.numerator // n.denominator
    root = isqrt(ni)
    exact = root * root == ni and n.denominator == 1
    # root = floor(sqrt(m)*2^k); keep one more bit of exactness for the nearest reading
    frac = Fraction(root) / (Fraction(2) ** k)
    if nearest and not exact:
        frac += Fraction(1) / (Fraction(2) ** (k + 1))     # strictly inside the interval: breaks the tie in favour of "above"
    return encode(frac, 0, nearest)


def _rsqrt_bits(a: Fraction, b: Fraction, nearest: bool = False) -> int:
    """a / sqrt(b) for b > 0, a != 0: exact value truncated to 24 bits (a single rounding, as double arithmetic does)."""
    s = 1 if a < 0 else 0
    a = abs(a)
    t = a * a / b                              # q^2
    l = _ilog2(t)
    e = l // 2                                 # 2^e <= q < 2^(e+1)
    n = t * (Fraction(4) ** (23 - e))          # floor(sqrt(n)) = floor(q * 2^(23-e))
    q = isqrt(n.numerator // n.denominator)
    if nearest:
        # compare q + 1/2 with the exact value: (q + 1/2)^2 against n
        half = Fraction((2 * q + 1) ** 2, 4)
        if half < n or (half == n and q & 1):
            q += 1
    if q >= 1 << 24:
        q >>= 1
        e += 1
    if e > 127:
        return (s << 31) | FMAX
    if e < -126:
        return s << 31
    return (s << 31) | ((e + 127) << 23) | (q & 0x7FFFFF)


def _sgn(x: int) -> int:
    return x >> 31


def _add_bits(a: int, b: int) -> int:
    """fpuDouble(a) + fpuDouble(b), rounded toward zero. a, b are bit patterns (clamped inside)."""
    a, b = clamp(a), clamp(b)
    s = val(a) + val(b)
    return encode(s, _sgn(a) & _sgn(b))


def _sub_bits(a: int, b: int) -> int:
    return _add_bits(a, clamp(b) ^ 0x80000000)


def _mul_bits(a: int, b: int) -> int:
    a, b = clamp(a), clamp(b)
    return encode(val(a) * val(b), _sgn(a) ^ _sgn(b))


def _raw_acc_add(acc: int, prod: int, sub: bool) -> int | None:
    """ACC += / -= prod, where ACC is used raw (FPU.cpp:MADDA_S, MSUBA_S): only the values the model can do exactly
    (an exponent that is not all ones); None for the others."""
    e = (acc >> 23) & 0xFF
    if e == 0xFF:
        return None
    acc = acc & 0x80000000 if e == 0 else acc          # a denormal ACC is a zero under denormals-are-zero
    p = prod ^ 0x80000000 if sub else prod
    return _add_bits(acc, p)


def _flags_ou(m, extra_clear=O | U):
    m.lo[FCR] &= ~extra_clear & M32


def _fmax(a: int, b: int) -> int:
    """fp_max (FPU.cpp:88): the bit patterns as signed integers; two negatives pick the smaller integer."""
    sa, sb = a - (1 << 32) if a >> 31 else a, b - (1 << 32) if b >> 31 else b
    return (min(a, b) if (sa < 0 and sb < 0) else (a if sa >= sb else b)) if True else 0


def _fmin(a: int, b: int) -> int:
    sa, sb = a - (1 << 32) if a >> 31 else a, b - (1 << 32) if b >> 31 else b
    return (max(a, b) if (sa < 0 and sb < 0) else (a if sa <= sb else b))


def _cvt_w(x: int) -> int:
    """CVT_W (FPU.cpp:CVT_W): exponent up to 0x9D converts with truncation, larger saturates by sign."""
    if (x & 0x7F800000) <= 0x4E800000:
        v = val_raw(x)
        n = int(v)                                   # Fraction -> int truncates toward zero
        return n & M32
    return 0x80000000 if x >> 31 else 0x7FFFFFFF


def val_raw(x: int) -> Fraction:
    """The exact value of a single without the clamp (a denormal is a zero: denormals-are-zero)."""
    e = (x >> 23) & 0xFF
    if e == 0:
        return Fraction(0)
    v = Fraction(0x800000 | (x & 0x7FFFFF)) * (Fraction(2) ** (e - 150))
    return -v if x >> 31 else v


def exec_cop1(m, word: int) -> None:
    fmt = (word >> 21) & 31
    rt = (word >> 16) & 31
    fs = (word >> 11) & 31
    fd = (word >> 6) & 31
    funct = word & 63
    ft = rt
    L = m.lo
    F = lambda r: L[F0 + r] & M32                    # noqa: E731
    setf = lambda r, v: L.__setitem__(F0 + r, v & M32)   # noqa: E731
    if fmt == 0x00:                                  # MFC1 (FPU.cpp:MFC1)
        if rt:
            L[rt] = sext32(F(fs))
    elif fmt == 0x02:                                # CFC1 (FPU.cpp:CFC1)
        if rt:
            L[rt] = sext32(L[FCR]) if fs == 31 else (0x2E00 if fs == 0 else 0)
    elif fmt == 0x04:                                # MTC1
        setf(fs, L[rt] & M32)
    elif fmt == 0x06:                                # CTC1: only FCR31, all 32 bits
        if fs == 31:
            L[FCR] = L[rt] & M32
    elif fmt == 0x10:
        _single(m, funct, fs, ft, fd, F, setf)
    elif fmt == 0x14:
        if funct == 0x20:                            # CVT.S.W: (float)(s32)Fs, truncated
            v = F(fs)
            sv = v - (1 << 32) if v >> 31 else v
            setf(fd, encode(Fraction(sv), 0))
        else:
            raise NotImplementedError(f"COP1.W {funct:#x}")
    else:
        raise NotImplementedError(f"COP1 fmt {fmt:#x}")


def _single(m, funct, fs, ft, fd, F, setf):
    L = m.lo
    fcr = lambda: L[FCR]                             # noqa: E731
    clr = lambda bits: L.__setitem__(FCR, L[FCR] & ~bits & M32)   # noqa: E731
    setb = lambda bits: L.__setitem__(FCR, (L[FCR] | bits) & M32)  # noqa: E731
    a, b = F(fs), F(ft)
    acc = L[ACC] & M32
    if funct == 0x00:   setf(fd, _add_bits(a, b));             clr(O | U)        # ADD_S
    elif funct == 0x01: setf(fd, _sub_bits(a, b));             clr(O | U)        # SUB_S
    elif funct == 0x02: setf(fd, _mul_bits(a, b));             clr(O | U)        # MUL_S
    elif funct == 0x03:                                                          # DIV_S
        if (b & 0x7F800000) == 0:
            setb((I | SI) if (a & 0x7F800000) == 0 else (D | SD))
            setf(fd, ((a ^ b) & 0x80000000) | FMAX)
        else:
            q = val(a) / val(b)
            setf(fd, encode(q, _sgn(clamp(a)) ^ _sgn(clamp(b)), nearest=(READING == 1)))
    elif funct == 0x04:                                                          # SQRT_S
        clr(I | D)
        if (b & 0x7F800000) == 0:
            setf(fd, b & 0x80000000)
        else:
            if b >> 31:
                setb(I | SI)
            setf(fd, _sqrt_bits(abs(val(b)), nearest=(READING == 1)))
    elif funct == 0x05: setf(fd, a & 0x7FFFFFFF);              clr(O | U)        # ABS_S
    elif funct == 0x06: setf(fd, a)                                              # MOV_S
    elif funct == 0x07: setf(fd, a ^ 0x80000000);              clr(O | U)        # NEG_S
    elif funct == 0x16:                                                          # RSQRT_S
        clr(D | I)
        if (b & 0x7F800000) == 0:
            setb(D | SD)
            setf(fd, (b & 0x80000000) | FMAX)
        else:
            if b >> 31:
                setb(I | SI)
            if (a & 0x7F800000) == 0:                     # 0 / x = 0, sign of the quotient
                setf(fd, (_sgn(clamp(a)) ^ 0) << 31)
            elif (b >> 31) or READING == 2:
                t = _sqrt_bits(abs(val(b)))               # temp.f = sqrt(|Ft|), a float
                setf(fd, encode(val(a) / val_raw(t), _sgn(clamp(a))))
            else:
                setf(fd, _rsqrt_bits(val(a), val(b), nearest=(READING == 1)))
    elif funct == 0x18: L[ACC] = _add_bits(a, b);              clr(O | U)        # ADDA_S
    elif funct == 0x19: L[ACC] = _sub_bits(a, b);              clr(O | U)        # SUBA_S
    elif funct == 0x1A: L[ACC] = _mul_bits(a, b);              clr(O | U)        # MULA_S
    elif funct == 0x1C:                                                          # MADD_S
        setf(fd, _add_bits(acc, _mul_bits(a, b)));             clr(O | U)
    elif funct == 0x1D:                                                          # MSUB_S
        setf(fd, _sub_bits(acc, _mul_bits(a, b)));             clr(O | U)
    elif funct in (0x1E, 0x1F):                                                  # MADDA_S, MSUBA_S
        r = _raw_acc_add(acc, _mul_bits(a, b), funct == 0x1F)
        if r is None:
            raise NotImplementedError("raw ACC with exponent 255")
        L[ACC] = r;                                            clr(O | U)
    elif funct == 0x28: setf(fd, _fmax(a, b));                 clr(O | U)        # MAX_S
    elif funct == 0x29: setf(fd, _fmin(a, b));                 clr(O | U)        # MIN_S
    elif funct == 0x24: setf(fd, _cvt_w(a))                                      # CVT_W
    elif funct == 0x30: clr(C)                                                   # C_F
    elif funct in (0x32, 0x34, 0x36):                                            # C_EQ, C_LT, C_LE (the codes the game uses)
        x, y = val(a), val(b)
        t = x == y if funct == 0x32 else (x < y if funct == 0x34 else x <= y)
        L[FCR] = (L[FCR] | C) if t else (L[FCR] & ~C & M32)
    else:
        raise NotImplementedError(f"COP1.S {funct:#x}")


def condition(m) -> bool:
    return bool(m.lo[FCR] & C)
