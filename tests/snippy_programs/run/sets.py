# Sets: in CPython's order, their operators and methods, frozenset.
s = {5, 3, 1, 3, 9}
print(s, len(s), 3 in s, 4 not in s, set(), set([4, 4, 2]), set("hello"), {x % 7 for x in range(20)})
s.add(2)
s.add(100)
s.discard(9)
s.remove(5)
print(s, s.pop(), s)
a = {1, 2, 3, 4}
b = {3, 4, 5, 6}
print(a | b, a & b, a - b, b - a, a ^ b, a.union([7, 8]), a.intersection(b, {4}), a.difference([1]), a.symmetric_difference(b))
print(a <= b, {1, 2} <= a, {1, 2} < a, a < a, a >= {1}, a.issubset(range(10)), a.issuperset([1, 2]), a.isdisjoint({9}))
c = set(a)
c |= {10}
c &= {1, 2, 10}
c -= {1}
c ^= {2, 3}
print(c)
f = frozenset([3, 1, 2])
print(f, f | {4}, {4} | f, hash(f) == hash(frozenset([1, 2, 3])), {f: "frozen"}[frozenset([1, 2, 3])])
print({-1, -2, -3, 0}, {10, 20, 30, 40, 50, 60, 70, 80, 90}, {2 ** 40, 7, 2 ** 33}, {0.5, 1.5, 2.0}, {True, 1, 1.0, 0, False})
big = set(range(100, 0, -7))
print(big)
print(sorted(big)[:5], len(big), min(big), max(big), sum(big))
words = {"apple", "banana", "cherry"}
print(sorted(words), "apple" in words, len(words))
empty = set()
try:
    empty.pop()
except KeyError as e:
    print("KeyError", e)
try:
    {1}.remove(2)
except KeyError as e:
    print("KeyError", e)
print(set([1, 2]) == {2, 1}, {1} == frozenset({1}), {(1, 2), (2, 1)}, {None, "a"} == {"a", None})
removed = set(range(20))
for i in range(0, 20, 3):
    removed.discard(i)
removed.add(30)
print(removed, removed.pop(), removed.pop(), removed)
