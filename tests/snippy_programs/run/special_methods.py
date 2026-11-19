# Special methods: operators and their reflected forms, comparisons, hashing, containers,
# calling, truth, repr and str, iteration, context, formatting.
class Vec:
    def __init__(self, x, y):
        self.x, self.y = x, y
    def __repr__(self):
        return f"Vec({self.x}, {self.y})"
    def __add__(self, other):
        if isinstance(other, Vec):
            return Vec(self.x + other.x, self.y + other.y)
        return NotImplemented
    def __radd__(self, other):
        if other == 0:
            return self
        return NotImplemented
    def __mul__(self, k):
        return Vec(self.x * k, self.y * k)
    def __rmul__(self, k):
        return self * k
    def __neg__(self):
        return Vec(-self.x, -self.y)
    def __abs__(self):
        return (self.x ** 2 + self.y ** 2) ** 0.5
    def __eq__(self, other):
        return isinstance(other, Vec) and (self.x, self.y) == (other.x, other.y)
    def __hash__(self):
        return hash((self.x, self.y))
    def __lt__(self, other):
        return abs(self) < abs(other)
    def __bool__(self):
        return bool(self.x or self.y)
    def __iter__(self):
        yield self.x
        yield self.y
    def __getitem__(self, i):
        return (self.x, self.y)[i]
    def __len__(self):
        return 2
    def __format__(self, spec):
        return f"<{self.x:{spec}}, {self.y:{spec}}>"
    def __iadd__(self, other):
        self.x += other.x
        self.y += other.y
        return self
v = Vec(3, 4)
w = Vec(1, 2)
print(v + w, v * 2, 3 * v, -v, abs(v), v == Vec(3, 4), v != w, v < w, w < v, sorted([v, w]), max(v, w))
print(sum([v, w]), bool(Vec(0, 0)), bool(v), list(v), v[1], len(v), {v: "v"}[Vec(3, 4)], f"{v:.1f}", format(w, "03d"))
x, y = v
print(x, y, Vec(1, 1) in [Vec(1, 1)], {Vec(1, 1), Vec(1, 1)})
u = v
u += w
print(u, v, u is v)
try:
    v + 1
except TypeError as e:
    print("TypeError:", e)
class Money:
    def __init__(self, cents):
        self.cents = cents
    def __str__(self):
        return f"${self.cents / 100:.2f}"
    def __repr__(self):
        return f"Money({self.cents})"
    def __lt__(self, o):
        return self.cents < o.cents
    def __le__(self, o):
        return self.cents <= o.cents
    def __gt__(self, o):
        return self.cents > o.cents
    def __ge__(self, o):
        return self.cents >= o.cents
m = Money(1050)
print(m, str(m), repr(m), [m], f"{m}", f"{m!r}", "%s %r" % (m, m), m > Money(5), m <= Money(5), Money(1) >= Money(1))
class Grid:
    def __init__(self):
        self.cells = {}
    def __getitem__(self, key):
        return self.cells.get(key, ".")
    def __setitem__(self, key, value):
        self.cells[key] = value
    def __delitem__(self, key):
        del self.cells[key]
    def __contains__(self, key):
        return key in self.cells
g = Grid()
g[0, 1] = "x"
g[2, 2] = "o"
print(g[0, 1], g[5, 5], (0, 1) in g, (9, 9) in g)
del g[0, 1]
print((0, 1) in g, g.cells)
class Adder:
    def __init__(self, n):
        self.n = n
    def __call__(self, x):
        return x + self.n
add5 = Adder(5)
print(add5(10), callable(add5), list(map(add5, [1, 2])))
class Countdown:
    def __init__(self, start):
        self.current = start
    def __iter__(self):
        return self
    def __next__(self):
        if self.current <= 0:
            raise StopIteration
        self.current -= 1
        return self.current + 1
print(list(Countdown(5)), [n for n in Countdown(3)], sum(Countdown(4)))
class OnlyEq:
    def __eq__(self, other):
        return True
try:
    hash(OnlyEq())
except TypeError as e:
    print("TypeError:", e)
print(OnlyEq() == 5, OnlyEq() != 5)
class Ordered:
    def __init__(self, v):
        self.v = v
    def __lt__(self, other):
        return self.v < other.v
    def __repr__(self):
        return f"O{self.v}"
print(sorted([Ordered(3), Ordered(1), Ordered(2)]), Ordered(1) > Ordered(0))
try:
    Ordered(1) <= Ordered(2)
except TypeError as e:
    print("TypeError:", e)
class Index:
    def __index__(self):
        return 2
print([10, 20, 30][Index()], "abc"[Index()], bin(Index()), range(10)[Index()])
class Num:
    def __int__(self):
        return 7
    def __float__(self):
        return 7.5
print(int(Num()), float(Num()))
