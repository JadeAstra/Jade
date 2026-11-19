# iter() and next() on Python's values and on objects of a program's own: __iter__ and __next__,
# __getitem__ alone, iter(callable, sentinel), iterators used up, and the builtins that take them.
class Squares:
    def __init__(self, n):
        self.n = n
    def __iter__(self):
        return SquareIter(self.n)
class SquareIter:
    def __init__(self, n):
        self.i = 0
        self.n = n
    def __iter__(self):
        return self
    def __next__(self):
        if self.i >= self.n:
            raise StopIteration
        self.i += 1
        return self.i * self.i
sq = Squares(4)
print(list(sq), list(sq), sum(sq), max(sq), 9 in sq, sorted(sq, reverse=True))
class Indexed:
    def __getitem__(self, i):
        if i >= 3:
            raise IndexError
        return "abc"[i]
print(list(Indexed()), "b" in Indexed(), [c.upper() for c in Indexed()])
it = iter([1, 2, 3])
print(next(it), list(it), list(it), next(it, "done"))
counter = [0]
def tick():
    counter[0] += 1
    return counter[0]
print(list(iter(tick, 4)))
d = {"a": 1, "b": 2}
di = iter(d)
print(next(di), list(di), list(iter(d.items())), list(reversed(d)))
print(list(enumerate("xy")), list(enumerate(["p", "q"], 10)), list(zip("ab", [1, 2], (True, False))), list(zip()))
print(list(map(len, ["a", "bb", "ccc"])), list(map(lambda a, b: a * b, [1, 2], [3, 4, 5])), list(filter(None, [0, 1, "", "x", None])))
print(list(filter(lambda n: n > 1, [0, 1, 2, 3])), list(reversed([1, 2, 3])), list(reversed("abc")), list(reversed(range(4))))
print(list(zip(*zip("abc", "123"))), dict(zip("ab", range(2))), list(map(str, range(3))))
m = map(lambda x: x * 10, range(3))
print(next(m), list(m))
z = zip("abc", "12")
print(list(z), list(z))
s = {3, 1, 2}
print(sorted(iter(s)), len(list(iter(s))))
for x, y in zip(range(3), "abc"):
    print(x, y, end="; ")
print()
nested = [[1, 2], [3], []]
flat = []
for inner in nested:
    for v in inner:
        flat.append(v)
print(flat)
gen = (x for x in range(5))
print(sum(gen), sum(gen))
try:
    list(zip([1, 2], [3], strict=True))
except ValueError as e:
    print("ValueError:", e)
print(any([]), all([]), any(x for x in []), min(iter([4, 2, 6])), max("hello", key=lambda c: -ord(c)))
r = iter(range(3))
print(next(r), next(r), next(r), next(r, None))
lst = [1, 2, 3]
for item in lst:
    if item == 2:
        lst.append(4)
    print("saw", item)
try:
    dd = {"x": 1}
    for k in dd:
        dd["y"] = 2
except RuntimeError as e:
    print("RuntimeError:", e)
