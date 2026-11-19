# The walrus operator, and match statements of every pattern kind.
data = [1, 5, 12, 7, 20]
if (n := len(data)) > 3:
    print("long list", n)
results = [y for x in data if (y := x * 2) > 10]
print(results, y)
chunks = iter(["a", "b", "", "c"])
while (chunk := next(chunks)) != "":
    print("chunk", chunk)
def classify(value):
    match value:
        case 0:
            return "zero"
        case 1 | 2 | 3:
            return "small"
        case int(n) if n < 0:
            return f"negative {n}"
        case int() | float():
            return "number"
        case "":
            return "empty string"
        case str(s):
            return f"string {s!r}"
        case [] :
            return "empty list"
        case [x]:
            return f"one item {x}"
        case [x, y]:
            return f"pair {x}, {y}"
        case [first, *rest]:
            return f"starts with {first}, then {len(rest)} more"
        case {"type": "point", "x": x, "y": y}:
            return f"point at {x}, {y}"
        case {"type": t, **others}:
            return f"type {t} with {sorted(others)}"
        case None:
            return "none"
        case _:
            return "something else"
for v in [0, 2, -5, 7.5, "", "hi", [], [9], [1, 2], [1, 2, 3, 4], {"type": "point", "x": 1, "y": 2}, {"type": "box", "w": 1, "h": 2}, None, (1, 2), {1, 2}]:
    print(repr(v), "->", classify(v))
class Point:
    __match_args__ = ("x", "y")
    def __init__(self, x, y):
        self.x = x
        self.y = y
def where(p):
    match p:
        case Point(0, 0):
            return "origin"
        case Point(x, 0):
            return f"on the x axis at {x}"
        case Point(x=0, y=y):
            return f"on the y axis at {y}"
        case Point(x, y) if x == y:
            return "on the diagonal"
        case Point():
            return "somewhere"
        case _:
            return "not a point"
for p in [Point(0, 0), Point(3, 0), Point(0, 4), Point(2, 2), Point(1, 5), "x"]:
    print(where(p))
def command(text):
    match text.split():
        case ["go", direction]:
            return f"going {direction}"
        case ["take", *items] if items:
            return f"taking {', '.join(items)}"
        case ["quit" | "exit"]:
            return "bye"
        case [("look" | "see") as verb]:
            return f"{verb}ing around"
        case _:
            return "unknown"
for t in ["go north", "take sword shield", "take", "quit", "exit", "look", "see", "dance now"]:
    print(t, "->", command(t))
match (1, "a"):
    case (int(), str()) as pair:
        print("tuple of int and str", pair)
match {"a": 1}:
    case {"b": _}:
        print("has b")
    case {"a": 1 | 2 as found}:
        print("a is", found)
match 42:
    case x:
        print("captured", x)
match True:
    case True:
        print("matched True")
