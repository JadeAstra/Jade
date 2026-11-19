# Dicts: insertion order, methods, views, comprehensions, merging, keys of many kinds.
d = {"b": 2, "a": 1, "c": 3}
print(d, len(d), d["a"], d.get("z"), d.get("z", 0), "a" in d, "z" not in d, list(d), list(d.keys()), list(d.values()), list(d.items()))
d["d"] = 4
d["a"] = 10
del d["b"]
print(d, d.pop("c"), d.pop("zz", "none"), d.setdefault("e", 5), d.setdefault("a", 99), d)
print(d.popitem(), d)
d.update({"x": 1}, y=2)
d.update([("z", 3)])
print(d, sorted(d), dict(sorted(d.items())))
keys = d.keys()
d["new"] = 0
print(keys, len(keys), "new" in keys, d.values(), d.items())
print({1: "int", 1.0: "float", 2: "two"}, {(1, 2): "tuple", None: "none", True: "bool"})
print(dict(a=1, b=2), dict([("x", 1), ("y", 2)]), dict({"k": "v"}, extra=1), dict.fromkeys("abc", 0), dict.fromkeys([1, 2]))
squares = {n: n * n for n in range(6) if n % 2 == 0}
print(squares, {v: k for k, v in squares.items()})
a = {"x": 1, "y": 2}
b = {"y": 20, "z": 30}
print(a | b, b | a, {**a, **b}, {**a, "w": 0})
a |= b
print(a)
counts = {}
for word in "the cat and the hat and the bat".split():
    counts[word] = counts.get(word, 0) + 1
print(counts, max(counts, key=counts.get), sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))
nested = {"a": {"b": {"c": 1}}}
nested["a"]["b"]["d"] = 2
print(nested, nested["a"]["b"])
for k, v in {"one": 1, "two": 2}.items():
    print(k, v)
print({} == {}, {"a": 1} == {"a": 1.0}, {"a": 1, "b": 2} == {"b": 2, "a": 1}, {"a": [1]} != {"a": [2]})
e = d.copy()
e.clear()
print(len(d) > 0, e, list(reversed({"p": 1, "q": 2})))
try:
    print(d["missing"])
except KeyError as err:
    print("KeyError", err, repr(err), err.args)
try:
    {[1]: 2}
except TypeError as err:
    print("TypeError", err)
