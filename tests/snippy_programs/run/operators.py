# Chained comparisons, is and in, augmented assignment on names, attributes and items, del,
# operator precedence.
a, b, c = 1, 2, 3
print(a < b < c, a < b > c, 1 < 2 < 3 < 4 < 5, 3 > 2 == 2, 1 == 1.0 == True, a is not None, [] is not [], None is None)
def noisy(v):
    print("evaluated", v)
    return v
print(noisy(1) < noisy(2) < noisy(3))
print(noisy(3) < noisy(2) < noisy(1))
x = [1, 2]
y = x
print(x is y, x == [1, 2], x is [1, 2], 2 in x, 3 not in x, "a" in "cat", "dog" not in "cat", (1, 2) in [(1, 2)])
n = 10
n += 5
n -= 3
n *= 2
n //= 5
n **= 3
n %= 7
n <<= 2
n >>= 1
n |= 8
n &= 13
n ^= 5
print(n)
f = 7.0
f /= 2
f -= 0.5
print(f)
s = "ab"
s *= 3
s += "!"
print(s)
lst = [1]
alias = lst
lst += [2, 3]
lst *= 2
print(lst, alias is lst)
t = (1,)
t_alias = t
t += (2,)
print(t, t_alias, t is t_alias)
class Box:
    def __init__(self):
        self.v = 1
        self.items = [0, 0]
b = Box()
b.v += 10
b.items[1] += 5
b.items[0] -= 1
d = {"k": 1}
d["k"] += 1
d.setdefault("new", 0)
d["new"] += 100
print(b.v, b.items, d)
grid = [[0] * 3 for _ in range(3)]
grid[1][1] += 9
print(grid)
v = 5
del v
try:
    print(v)
except NameError as e:
    print("NameError:", e)
items = list(range(10))
del items[0], items[-1]
del items[::2]
print(items)
dd = {"a": 1, "b": 2}
del dd["a"]
print(dd)
class Obj:
    pass
o = Obj()
o.x = 1
del o.x
print(hasattr(o, "x"))
print(2 + 3 * 4 ** 2 // 5 - 1, -2 ** 2, (-2) ** 2, not 1 == 2, 1 + 2 == 3 and 4 > 3, 5 & 3 | 8 ^ 1, ~0 + 1, 2 ** 3 ** 2)
print(10 - 2 - 3, 2 ** -1, 100 / 10 / 5, 7 % 3 * 2, "a" + "b" * 2, [1] * 2 + [3], 1 if 0 else 2 if 0 else 3)
print(True and "x", False or "y", not "", 0 or [] or {} or "last", 1 and 2 and 3, None and 1)
print(1 < 2 and 2 < 3 or False, (1, 2) < (1, 3) < (2, 0), "abc" < "abd" <= "abd")
p = q = 0
p, q = q + 1, p + 2
print(p, q)
