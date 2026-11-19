# Python's numeric tower: bool, int, float and complex together; ints past 64 bits in every
# operation; floats at their edges; comparisons across kinds.
values = [True, 7, -3, 2 ** 70, 2.5, -0.5, 1 + 2j]
for a in values[:6]:
    print(a, a + 1, a * 2, a - True, -a, abs(a), a == int(a) if not isinstance(a, complex) else "", a ** 2)
print(2 ** 70 + 0.5, 2 ** 70 * 1.0, 2 ** 70 // 3, 2 ** 70 % 7, -(2 ** 70) // 7, divmod(-(2 ** 70), 7), 2 ** 70 / 2 ** 69)
print((2 ** 70) << 3, (2 ** 70) >> 65, (2 ** 70) & 0xFFFF, (2 ** 70) | 1, (2 ** 70) ^ (2 ** 70), ~(2 ** 70))
print(10 ** 30 // 10 ** 15, 10 ** 30 % 999, pow(2, 100, 10 ** 9), int(str(2 ** 200)) == 2 ** 200, len(str(3 ** 1000)))
print(2 ** 63 == 2.0 ** 63, 2 ** 64 > 1.8e19, 2 ** 1000 > 1e300, 10 ** 400 > float("inf"), -(10 ** 400) < float("-inf"))
print(1 + 2j + 3, (1 + 2j) * 2.5, (1 + 2j) / 2, abs(3 + 4j), (1 + 2j) == 1 + 2j, (2 + 0j) == 2, complex(True))
print(sum([1, 2.5, True]), sum([2 ** 64, 2 ** 64]), max(1, 1.0), min(1.0, 1), max(True, 1), sorted([1.5, 1, True, 0, -2 ** 70]))
print(7 / 7, 6 / 4, -7 / 2, 1e308 * 10, -1e308 * 10, 1e-320 / 1e10, 0.1 + 0.2 == 0.3, abs(0.1 + 0.2 - 0.3) < 1e-9)
print(int(1e15), int(1e16), int(-1e17), float(2 ** 53 + 1), float(10 ** 20), round(1e20), round(2 ** 70 / 3))
print(3 * True, True * "ab", False * [1], 5 // True, 5 % True, 2 ** True, True / 4, -True, +False)
print(hash(2 ** 70) == hash(2.0 ** 70), hash(0.5), hash(-0.5), hash(1e100) == hash(int(1e100)), {1: "a", 1.0: "b", True: "c"})
print(0.1 * 3, 1 / 3 + 1 / 3 + 1 / 3, 2 / 3, 4.35 * 100, 1.1 + 2.2, 3.3 * 3, 1e-7 * 3, 123456.789e3)
x = 1
for _ in range(100):
    x *= 3
print(x, x.bit_length(), x % 1000, str(x)[:20])
print(isinstance(2 ** 70, int), type(2 ** 70).__name__, (2 ** 70).bit_length(), int("1" * 25), -int("9" * 25))
