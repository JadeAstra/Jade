# range, and unpacking in assignments and loops.
print(range(5), list(range(5)), list(range(2, 10, 3)), list(range(10, 0, -3)), list(range(0)), range(1, 10, 2))
r = range(1, 20, 4)
print(len(r), r[0], r[-1], r[1:3], 9 in r, 10 in r, r.index(13), r.count(5), list(reversed(r)), r == range(1, 18, 4))
print(sum(range(101)), max(range(7)), list(range(-5, -1)), range(5)[::-1], list(range(5)[::2]))
for i in range(3):
    print("i", i)
for i in range(10, 0, -4):
    print("down", i)
for i in range(2, 2):
    print("never")
n = 3
for i in range(n):
    n = 10
    print("n changed", i)
a, b, c = "xyz"
print(a, b, c)
(a, b), c = [1, 2], 3
print(a, b, c)
first, *others = range(5)
print(first, others)
*front, back = "hello"
print(front, back)
a, *b, c = 1, 2
print(a, b, c)
x = y = [1]
x.append(2)
print(x, y)
for k, v in [("a", 1), ("b", 2)]:
    print(k, v)
for (i, j), k in [((1, 2), 3), ((4, 5), 6)]:
    print(i, j, k)
data = [[1, 2, 3], [4, 5, 6]]
for first, *rest in data:
    print(first, rest)
try:
    a, b = [1, 2, 3]
except ValueError as e:
    print("ValueError", e)
try:
    a, b, c = [1, 2]
except ValueError as e:
    print("ValueError", e)
try:
    a, b = 5
except TypeError as e:
    print("TypeError", e)
try:
    a, *b, c = [1]
except ValueError as e:
    print("ValueError", e)
i = 0
values = [0, 0, 0]
i, values[i] = 1, 9
print(i, values)
