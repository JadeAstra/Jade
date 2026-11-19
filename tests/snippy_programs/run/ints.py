# Python's ints: of any size, floor division and modulo, powers, shifts, bits.
big = 2 ** 64
print(big, big * big, big - 1, -big, big // 3, big % 7)
print(9223372036854775807 + 1, -9223372036854775808 - 1, 9223372036854775807 * 2)
print(3037000500 * 3037000500, (2**62) * 4, (-2**63) // -1)
for a in (7, -7):
    for b in (2, -2):
        print(a, b, a // b, a % b, divmod(a, b))
print(2 ** 10, 2 ** -1, (-2) ** 3, 0 ** 0, pow(2, 10, 1000), pow(3, -1, 7))
print(1 << 10, 1 << 70, -1 << 3, 1024 >> 3, -1024 >> 3, -1 >> 100, (2**100) >> 90)
print(5 & 3, 5 | 3, 5 ^ 3, ~5, ~-1, -5 & 0xFF, (2**70) | 1, (2**70) & (2**70 + 5))
print(abs(-5), abs(-2**70), int("42"), int("-0x1F", 16), int("0b101", 0), int("1_000_000"))
print((255).bit_length(), (-255).bit_length(), (2**100).bit_length(), (7).bit_count())
print(10 // 3, 10.0 // 3, -10 // 3.0, 10 % 3.5, 2 ** 0.5)
print(True + True, True * 10, False - 1, True / 2, True << 3)
n = 1
for i in range(1, 30):
    n *= i
print(n, n // 10**20, n % 1000003)
print(int(3.9), int(-3.9), int(1e20), round(2.5), round(3.5), round(-0.5), round(1234, -2), round(1250, -2))
print(0x7fffffffffffffff, 0o777, 0b1010, 1_000)
print(hex(255), oct(8), bin(5), hex(-255), hex(2**70))
