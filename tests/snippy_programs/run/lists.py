# Lists: their operations, methods, slices and slice assignment, del, sort.
a = [3, 1, 4, 1, 5, 9, 2, 6]
print(a, len(a), a[0], a[-1], a[2:5], a[::-1], a[::3], a[-3:], a[100:])
a.append(5)
a.extend([3, 5])
a.insert(0, 0)
a.insert(-1, 99)
a.insert(100, 100)
print(a)
print(a.pop(), a.pop(0), a.pop(-2), a)
a.remove(1)
print(a, a.index(5), a.index(5, 5), a.count(5), 9 in a, 7 in a)
b = sorted(a)
a.sort()
print(a == b, a, sorted(a, reverse=True))
words = ["banana", "Apple", "cherry", "date"]
print(sorted(words), sorted(words, key=str.lower), sorted(words, key=len), sorted(words, key=lambda w: (-len(w), w)))
nums = [5, 2, 8]
nums.reverse()
print(nums, nums * 2, nums + [1], [0] * 5, [[0] * 2] * 2)
c = list(range(10))
c[2:5] = ["a", "b"]
print(c)
c[::2] = [None] * len(c[::2])
print(c)
del c[0]
del c[-2:]
del c[::2]
print(c)
d = [1, 2, 3]
e = d
e += [4]
print(d, e, d is e)
f = d
f = f + [5]
print(d, f, d is f)
m = [[1, 2], [3, 4]]
m[0][1] = 20
print(m, [row[1] for row in m], [x for row in m for x in row])
print([1, 2] < [1, 3], [1, 2] < [1, 2, 0], [2] > [1, 9], [1, 2] == [1, 2], [] == [], [1, 2] != [1, 2.0])
copy = d.copy()
copy.clear()
print(d, copy, list("abc"), list((1, 2)), list({1: "a", 2: "b"}))
lst = [1, 2, 3]
lst[1:1] = [9, 9]
lst[-1:] = []
print(lst, lst[1:-1], max(lst), min(lst), sum(lst))
x = [1, [2, [3, [4]]]]
print(x, x[1][1][1][0], len(x))
x.append(x)
print(x)
