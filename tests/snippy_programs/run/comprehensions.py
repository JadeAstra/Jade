# List, set and dict comprehensions and generator expressions: nesting, conditions, scopes.
print([x * x for x in range(10)])
print([x for x in range(20) if x % 3 == 0 if x % 2 == 0])
print([(x, y) for x in range(3) for y in range(x)])
print([[x * y for y in range(4)] for x in range(3)])
print({x % 5 for x in range(20)}, {c.upper() for c in "hello"} == {"H", "E", "L", "O"})
print({k: v for k, v in zip("abc", range(3))}, {x: x ** 2 for x in range(5) if x != 2})
print(list(x * 2 for x in range(5)), sum(x for x in range(101)), max(len(w) for w in ["a", "abc", "ab"]))
print(any(x > 3 for x in [1, 2, 5]), all(x > 0 for x in [1, 2, -1]), sorted(c for c in "banana"))
x = "outer"
squares = [x for x in range(3)]
print(x, squares)
matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
print([row[i] for i in range(3) for row in matrix], [[row[i] for row in matrix] for i in range(3)])
print([n for row in matrix for n in row if n % 2])
funcs = [lambda: i for i in range(3)]
print([f() for f in funcs])
funcs2 = [lambda i=i: i for i in range(3)]
print([f() for f in funcs2])
gen = (n * n for n in range(4))
print(next(gen), next(gen), list(gen), list(gen))
words = ["apple", "Banana", "cherry"]
print({w[0].lower(): w for w in words}, [w for w in words if w[0].isupper()])
nested = [[1, [2, 3]], [4, [5]]]
print([y for a, bs in nested for y in [a] + bs])
print([i if i % 2 else -i for i in range(6)], [(i, j) for i in range(2) for j in "ab"])
total = 0
print([total := total + v for v in [1, 2, 3]], total)
class Holder:
    values = [1, 2, 3]
    doubled = [v * 2 for v in values]
print(Holder.doubled)
def make():
    base = 10
    return [base + i for i in range(3)], {base * i for i in range(3)}, (base - i for i in range(2))
lst, st, gn = make()
print(lst, sorted(st), list(gn))
print(list(zip(*[[1, 2, 3], [4, 5, 6]])), [list(t) for t in zip("ab", "cd")])
print(dict((str(i), i) for i in range(3)), set(c for c in "aab"), tuple(i for i in range(3)))
print([c for c in "a1b2c3" if c.isdigit()], "".join(c for c in "hello" if c not in "aeiou"))
