# Builtins' names given other values, a module's own names, __name__, and sorting: stable, by
# keys, reversed, of mixed numbers, with ties.
print(__name__, __doc__)
list_backup = list
def list(*args):
    return "my list"
print(list(), list_backup("ab"))
del list
print(list("cd"))
max = 0
for v in [3, 9, 4]:
    if v > max:
        max = v
print(max)
del max
print(max([1, 2]))
def uses_len(x):
    return len(x)
len = lambda x: -1
print(uses_len([1, 2, 3]))
del len
print(uses_len([1, 2, 3]))
print = print
print("print still prints")
records = [("bob", 3), ("amy", 1), ("cat", 3), ("dan", 2), ("eve", 1)]
print(sorted(records, key=lambda r: r[1]))
print(sorted(records, key=lambda r: r[1], reverse=True))
print(sorted(records, key=lambda r: (-r[1], r[0])))
by_name = sorted(records)
print(sorted(by_name, key=lambda r: r[1]))
nums = [3, 1.5, 2, -1, 2.0, True, 0]
print(sorted(nums), sorted(nums, reverse=True), sorted(nums, key=abs))
words = "The quick brown fox jumps over the lazy dog".split()
print(sorted(words), sorted(words, key=str.lower), sorted(words, key=len), sorted(words, key=lambda w: w[-1]))
lst = [5, 2, 8, 1]
lst.sort()
print(lst)
lst.sort(reverse=True)
print(lst)
pairs = [[2, "b"], [1, "a"], [2, "a"]]
pairs.sort()
print(pairs)
big = [(i * 7919) % 101 for i in range(100)]
big.sort()
print(big[:10], big[-5:])
stable = sorted(range(20), key=lambda i: i % 3)
print(stable)
print(sorted("hello world"), sorted({3: "c", 1: "a"}.items()), sorted(set("mississippi")))
print(min(records, key=lambda r: r[1]), max(records, key=lambda r: r[1]))
