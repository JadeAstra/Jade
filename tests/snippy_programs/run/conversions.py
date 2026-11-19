# Converting between Python's types: int(), float(), str(), bool(), complex(), list(), tuple(),
# dict(), set(), and text to numbers with their errors.
print(int("42"), int(" -7 "), int("+3"), int("1_000"), int("ff", 16), int("0xff", 0), int("777", 8), int("z", 36), int("0b11", 2))
print(int(3.99), int(-3.99), int(True), int("١٢٣"), int(2 ** 80 + 0.0))
print(float("3.5"), float("-1e3"), float("inf"), float("  2.5e-3  "), float(7), float("1_0.5"), float(".5"), float("5."))
print(str(1), str(1.0), str(-0.0), str(True), str([1, 2]), str((1,)), str({"a": 1}), str({1}), str(None), str(1e100))
print(bool(0), bool(0.0), bool(""), bool(" "), bool([]), bool([[]]), bool({}), bool(None), bool(range(0)))
print(complex(1), complex(1.5, -2), complex("3+4j"), complex("-2j"), complex("1e1+1e1j"))
print(list("abc"), list(range(3)), list((1, 2)), list({"x": 1}), tuple([1, 2]), tuple("ab"), set("abca") == {"a", "b", "c"})
print(dict([(1, 2), (3, 4)]), dict(zip("abc", range(3))), dict(a=1, b=2), dict({"x": 1}, y=2), dict([("k", "v")], k="override"))
print(list(map(int, "123")), [float(x) for x in ["1", "2.5"]], int(float("2.9")), str(int("08")))
for bad in ["abc", "1.5", "", " ", "1e3", "0x10", "--1", "1__0", "_1"]:
    try:
        int(bad)
    except ValueError as e:
        print("int:", e)
for bad in ["abc", "", "1.2.3", "e5", "inf inity"]:
    try:
        float(bad)
    except ValueError as e:
        print("float:", e)
try:
    int("12", 1)
except ValueError as e:
    print("ValueError:", e)
try:
    int(1.5, 10)
except TypeError as e:
    print("TypeError:", e)
try:
    int(float("nan"))
except ValueError as e:
    print("ValueError:", e)
try:
    int(float("inf"))
except OverflowError as e:
    print("OverflowError:", e)
print(round(float("1e16")), int("9" * 30), float("9" * 30), int(-0.0), 0.1 + 0.7)
print(str(12345678901234567890), repr(1e22), repr(1e-4), repr(123456789.0), repr(0.1 * 3))
print(bin(10), int(bin(10), 2), hex(255), int(hex(255), 16), oct(8), int(oct(8), 8))
print(ord("A"), chr(ord("A") + 1), [ord(c) for c in "hi"], "".join(chr(c) for c in [72, 105]))
print(list(str(123)), int("".join(["4", "2"])), float(str(0.1)) == 0.1, str(float(10)))
