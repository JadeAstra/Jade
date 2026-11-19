# Matrices and vectors as classes with operators: multiplication, determinants by expansion,
# transposes, fractions of their own for exact inverses, and printing aligned.
class Frac:
    def __init__(self, n, d=1):
        if d == 0:
            raise ZeroDivisionError("zero denominator")
        def gcd(a, b):
            while b:
                a, b = b, a % b
            return a
        g = gcd(abs(n), abs(d)) or 1
        sign = -1 if d < 0 else 1
        self.n, self.d = sign * n // g, abs(d) // g
    def __add__(self, o):
        o = o if isinstance(o, Frac) else Frac(o)
        return Frac(self.n * o.d + o.n * self.d, self.d * o.d)
    __radd__ = __add__
    def __sub__(self, o):
        o = o if isinstance(o, Frac) else Frac(o)
        return Frac(self.n * o.d - o.n * self.d, self.d * o.d)
    def __mul__(self, o):
        o = o if isinstance(o, Frac) else Frac(o)
        return Frac(self.n * o.n, self.d * o.d)
    __rmul__ = __mul__
    def __truediv__(self, o):
        o = o if isinstance(o, Frac) else Frac(o)
        return Frac(self.n * o.d, self.d * o.n)
    def __neg__(self):
        return Frac(-self.n, self.d)
    def __eq__(self, o):
        o = o if isinstance(o, Frac) else Frac(o)
        return self.n == o.n and self.d == o.d
    def __hash__(self):
        return hash((self.n, self.d))
    def __repr__(self):
        return str(self.n) if self.d == 1 else f"{self.n}/{self.d}"
class Matrix:
    def __init__(self, rows):
        self.rows = [list(r) for r in rows]
    def __mul__(self, other):
        cols = list(zip(*other.rows))
        return Matrix([[sum((a * b for a, b in zip(r, c)), Frac(0)) for c in cols] for r in self.rows])
    def det(self):
        m = self.rows
        if len(m) == 1:
            return m[0][0]
        total = Frac(0)
        for j, v in enumerate(m[0]):
            minor = Matrix([row[:j] + row[j + 1:] for row in m[1:]])
            total = total + (v if j % 2 == 0 else -v) * minor.det()
        return total
    def transpose(self):
        return Matrix(zip(*self.rows))
    def inverse(self):
        n = len(self.rows)
        aug = [list(r) + [Frac(1 if i == j else 0) for j in range(n)] for i, r in enumerate(self.rows)]
        for col in range(n):
            pivot = next(r for r in range(col, n) if aug[r][col] != 0)
            aug[col], aug[pivot] = aug[pivot], aug[col]
            p = aug[col][col]
            aug[col] = [x / p for x in aug[col]]
            for r in range(n):
                if r != col:
                    f = aug[r][col]
                    aug[r] = [a - f * b for a, b in zip(aug[r], aug[col])]
        return Matrix([row[n:] for row in aug])
    def __eq__(self, other):
        return self.rows == other.rows
    def __str__(self):
        cells = [[repr(x) for x in r] for r in self.rows]
        width = max(len(c) for r in cells for c in r)
        return "\n".join(" ".join(c.rjust(width) for c in r) for r in cells)
m = Matrix([[Frac(2), Frac(1), Frac(1)], [Frac(1), Frac(3), Frac(2)], [Frac(1), Frac(0), Frac(0)]])
print(m)
print(m.det(), m.transpose().det(), Frac(1, 2) + Frac(1, 3), Frac(3, 6) == Frac(1, 2), Frac(4, -6))
inv = m.inverse()
print(inv)
identity = m * inv
print(identity)
print(identity == Matrix([[Frac(1 if i == j else 0) for j in range(3)] for i in range(3)]))
try:
    Frac(1, 0)
except ZeroDivisionError as e:
    print("ZeroDivisionError:", e)
print(sum([Frac(1, n) for n in range(1, 7)], Frac(0)), {Frac(1, 2), Frac(2, 4), Frac(1, 3)} == {Frac(1, 2), Frac(1, 3)})
