# Tuples: packing, unpacking, comparing, hashing, counting.
t = (1, 2, 3)
single = (1,)
empty = ()
print(t, single, empty, len(t), t[1], t[-1], t[1:], t[::-1], type(single), (1), tuple([4, 5]))
a, b, c = t
print(a, b, c)
x, *rest = t
*init, last = t
first, *middle, final = (1, 2, 3, 4, 5)
print(x, rest, init, last, first, middle, final)
print((1, 2) + (3,), (1, 2) * 2, (1, 2) < (1, 3), (1, 2) < (1, 2, 0), (2,) > (1, 99), (1, 2) == (1, 2), (1, 2) == [1, 2])
print(t.count(2), t.index(3), 2 in t, hash((1, 2)) == hash((1, 2)), {(1, 2): "pair"}[(1, 2)])
nested = ((1, 2), (3, (4, 5)))
(p, q), (r, (s, u)) = nested
print(p, q, r, s, u)
for i, (name, score) in enumerate([("a", 1), ("b", 2)]):
    print(i, name, score)
print(tuple("abc"), tuple(range(3)), tuple(x * x for x in range(4)), (1, "a", None, 2.5))
pairs = [(2, "b"), (1, "z"), (2, "a")]
print(sorted(pairs), max(pairs), min(pairs))
def coords():
    return 1, 2
xs, ys = coords()
print(xs, ys, coords())
