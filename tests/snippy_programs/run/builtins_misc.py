# The builtins that are not types: min, max, sum, sorted, abs, round, divmod, pow, any, all,
# chr, ord, hex, oct, bin, hash, isinstance, issubclass, callable, len, repr, ascii, format, id,
# type, vars, dir, getattr and friends.
print(min(3, 1, 2), max([3, 1, 2]), min("b", "a"), max([], default=0), min([5], key=lambda x: -x), max(range(10), key=lambda n: n % 4))
print(sum([1, 2, 3]), sum([1, 2], 10), sum([0.1] * 10), sum([[1], [2]], []), sum(range(5), start=1))
print(sorted([3, 1, 2]), sorted("cab"), sorted([3, 1, 2], reverse=True), sorted({"b": 1, "a": 2}), sorted([(1, "b"), (1, "a")]))
print(abs(-3), abs(3.5), abs(-0.0), round(3.14159, 2), round(2.5), round(-2.5), round(7, -1), round(15, -1), round(25, -1))
print(divmod(17, 5), divmod(-17, 5), divmod(17.5, 5), pow(2, 8), pow(2, 8, 5), pow(2.0, 3), pow(-1, 0.5) if False else 0)
print(any([0, "", None]), any([0, 1]), all([1, "a"]), all([1, 0]), all([]), any([]))
print(chr(97), ord("a"), chr(8364), ord("€"), hex(255), oct(255), bin(255), hex(0), bin(-3))
print(hash(1) == hash(1.0), hash("a") == hash("a"), hash((1, "a")) == hash((1, "a")), hash(True), hash(0), hash(-1), hash(2 ** 61 - 1), hash(2 ** 61))
print(isinstance(1, int), isinstance(True, int), isinstance(1, (str, int)), isinstance("x", str), isinstance(1.0, int), isinstance(None, type(None)))
print(issubclass(bool, int), issubclass(int, object), issubclass(str, (int, str)), issubclass(ValueError, Exception), issubclass(KeyError, LookupError))
print(callable(len), callable("x"), callable(lambda: 0), callable(int), len("abc"), len([1, 2]), len({1: 2}), len(range(10)))
print(repr("a\nb"), repr(1.0), repr([1, "x"]), ascii("é→x"), format(3.14159, ".2f"), format(42, "b"), format("x", "^5"), format(1234567, ","))
print(type(1) is int, type(type) is type, type(None).__name__, type(len).__name__, type(lambda: 0).__name__, type(print).__name__)
class P:
    def __init__(self):
        self.a = 1
        self.b = 2
p = P()
print(vars(p), getattr(p, "a"), getattr(p, "z", None), hasattr(p, "b"), [n for n in dir(p) if not n.startswith("__")])
setattr(p, "c", 3)
delattr(p, "a")
print(vars(p), id(p) == id(p), id(p) != id(P()))
print(int.__name__, str.upper("abc"), list.__name__, dict.fromkeys("ab"), str.join(",", ["a", "b"]), int("ff", 16))
print(max(1, 2.5), min(-1, -1.5), max("apple", "banana", key=len), sorted(["b", "A", "c"], key=str.lower))
print(round(0.5), round(1.5), round(2.675, 2), round(-1.005, 2), round(123.456, -1), round(5e-324, 400) == 5e-324)
print(pow(3, -1, 11), pow(10, 2), 2 ** -2, (-8) // 3, -8 % 3, 7 // -2)
print(list(range(3)) == [0, 1, 2], tuple(range(2)), set(range(3)), frozenset(range(2)), dict(a=1))
print(bool([]), bool([0]), int(True), float(False), str(None), str(b"x") if False else "bytes-str")
print(sorted([1, 3, 2], key=None), reversed is not None, enumerate is not None, zip is not None)
