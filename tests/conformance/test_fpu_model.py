"""The floating-point model's own arithmetic against values worked out by hand (T-210 piece 5).

The model computes exactly (fractions, integer square roots) and truncates to 24 bits; nothing here depends on the
rounding mode of the Python interpreter or of the host.
"""
from fractions import Fraction

import fpu_ref as m


class M:
    def __init__(self):
        self.lo = [0] * 71


def run(funct, a, b=0, acc=0, fcr=0):
    s = M()
    s.lo[m.F0 + 1], s.lo[m.F0 + 2], s.lo[m.ACC], s.lo[m.FCR] = a, b, acc, fcr
    m.exec_cop1(s, (0x11 << 26) | (0x10 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | funct)
    return s.lo[m.F0 + 3], s.lo[m.ACC], s.lo[m.FCR]


def test_sum_is_truncated_not_rounded():
    # 1 + 0.75 ulp: truncation gives 1.0, round-to-nearest would give 1 + ulp
    tiny = 0x33400000                      # 3 * 2^-25
    assert run(0x00, 0x3F800000, tiny)[0] == 0x3F800000
    assert m.encode(Fraction(1) + Fraction(3, 2 ** 25), nearest=True) == 0x3F800001


def test_division_one_third():
    assert run(0x03, 0x3F800000, 0x40400000)[0] == 0x3EAAAAAA        # 0.33333331 (nearest would be ...AB)


def test_square_root_of_five():
    assert m._sqrt_bits(Fraction(5)) == 0x400F1BBC                   # 2.2360678 (nearest: ...BD)
    assert m._sqrt_bits(Fraction(5), nearest=True) == 0x400F1BBD
    assert m._sqrt_bits(Fraction(4)) == 0x40000000                   # exact roots are exact
    assert run(0x04, 0, 0x41100000)[0] == 0x40400000                 # sqrt(9) = 3


def test_signs_of_zero():
    assert run(0x00, 0x80000000, 0x80000000)[0] == 0x80000000        # -0 + -0 = -0
    assert run(0x00, 0x00000000, 0x80000000)[0] == 0x00000000        # +0 + -0 = +0
    assert run(0x02, 0x80000000, 0x3F800000)[0] == 0x80000000        # -0 * 1 = -0


def test_overflow_underflow_and_clamps():
    assert run(0x02, 0x7F7FFFFF, 0x40000000)[0] == 0x7F7FFFFF        # largest * 2 = largest
    assert run(0x02, 0xFF7FFFFF, 0x40000000)[0] == 0xFF7FFFFF
    assert run(0x02, 0x00800000, 0x3F000000)[0] == 0x00000000        # smallest normal / 2 flushes to zero
    assert run(0x00, 0x7F800000, 0x3F800000)[0] == 0x7F7FFFFF        # exponent all ones reads as the largest number


def test_division_by_zero():
    r, _, fcr = run(0x03, 0x3F800000, 0x80000000)
    assert r == 0xFF7FFFFF and fcr == 0x10020                        # D | SD, sign of the operands
    r, _, fcr = run(0x03, 0, 0)
    assert r == 0x7F7FFFFF and fcr == 0x20040                        # I | SI


def test_conversions():
    assert run(0x24, 0x4F000000)[0] == 0x7FFFFFFF                    # 2^31 saturates
    assert run(0x24, 0xCF000000)[0] == 0x80000000                    # -2^31 is exact
    assert run(0x24, 0xCF000001)[0] == 0x80000000                    # below it saturates
    assert run(0x24, 0xBF7FFFFF)[0] == 0                             # -0.99999994 truncates to 0
    assert run(0x24, 0x40E00000)[0] == 7                             # 7.0
    assert m.encode(Fraction(0x01000001)) == 0x4B800000              # 16777217 truncates to 2^24
