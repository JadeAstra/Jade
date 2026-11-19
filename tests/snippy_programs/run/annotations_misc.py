# What ordinary scripts also write: type annotations (kept, not checked), constants, __slots__,
# chained methods, if __name__ blocks, semicolons, backslash continuations, pass and ... bodies.
MAX_ITEMS: int = 3
names: list[str] = ["a", "b"]
counter: int
def scale(values: list[float], factor: float = 2.0) -> list[float]:
    result: list[float] = []
    for v in values:
        result.append(v * factor)
    return result
print(scale([1, 2.5]), scale([1], factor=0.5), MAX_ITEMS, names)
def anything(x: "SomethingNotDefined") -> "AlsoNot":
    return x
print(anything(5))
class Config:
    debug: bool = False
    level: int
    def __init__(self, level: int) -> None:
        self.level = level
c = Config(3)
print(c.debug, c.level, Config.debug)
x = 1; y = 2; z = x + y
print(x, y, z)
total = 1 + \
    2 + \
    3
print(total)
long_list = [
    1, 2,
    3,   # a comment inside
]
print(long_list, (
    "parenthesized"
))
def todo():
    ...
class Placeholder:
    pass
print(todo(), Placeholder.__name__, type(...).__name__, ... is Ellipsis)
if __name__ == "__main__":
    print("run as the program")
s = " Mixed Case "
print(s.strip().lower().replace(" ", "_").upper())
values = {"b": 2, "a": 1}
print(sorted(values.items())[0][1], len(values) and "nonempty", [] or "empty")
i = 0
while i < 10 and not (i > 3 and i % 2 == 0):
    i += 1
else:
    print("stopped at", i)
def defaults(a, b=[], *, c={}):
    b.append(a)
    c[a] = len(b)
    return b, c
print(defaults(1), defaults(2))
lambda_table = {"double": lambda n: n * 2, "square": lambda n: n ** 2}
print({k: f(7) for k, f in lambda_table.items()})
print(int("7") + float("0.5"), str(3) * 2, bool("False"), list(map(int, "123")))
