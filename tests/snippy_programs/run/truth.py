# Truth, and/or giving their operands, not, bool arithmetic, None.
values = [0, 1, -1, 0.0, 2.5, "", "a", [], [0], (), (0,), {}, {0: 0}, set(), None, True, False, range(0), range(3)]
for v in values:
    print(repr(v), bool(v), not v)
print(1 and 2, 0 and 2, 1 or 2, 0 or 2, None or "default", "" or [] or 0)
print([] and [1], [1] and [2], (0 or 0.0), (None and 1))
x = None
print(x is None, x is not None, x == None)
print(True + 1, True == 1, False == 0, True is True, isinstance(True, int))
print(sum([True, True, False]), True & False, True | False, True ^ True)
class Empty:
    def __len__(self):
        return 0
class Falsy:
    def __bool__(self):
        return False
print(bool(Empty()), bool(Falsy()), not Empty(), Empty() or "fallback")
