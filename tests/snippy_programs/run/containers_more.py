# More of the containers: nested and aliased, copied shallow and deep by hand, compared,
# unpacked into calls, used as stacks and queues, self-referencing, sorted in place.
a = [[1, 2], [3, 4]]
shallow = a[:]
deep = [row[:] for row in a]
a[0][0] = 99
print(shallow, deep, a == shallow, a is shallow, a[0] is shallow[0])
grid = [[0] * 3] * 3
grid[0][0] = 1
print(grid)
matrix = [[i * 3 + j for j in range(3)] for i in range(3)]
print(matrix, [list(r) for r in zip(*matrix)], [matrix[i][i] for i in range(3)], sum(map(sum, matrix)))
def total(*nums, scale=1):
    return sum(nums) * scale
args = [1, 2, 3]
opts = {"scale": 10}
print(total(*args), total(*args, **opts), total(*range(5), *args), total(**{"scale": 2}))
stack = []
for ch in "abc":
    stack.append(ch)
print(stack.pop(), stack.pop(), stack)
queue = list(range(5))
print(queue.pop(0), queue.pop(0), queue)
d = {}
d["self"] = d
print(d)
l = [1]
l.append(l)
print(l, len(l))
pairs = list(zip("abc", range(3)))
pairs.sort(key=lambda p: -p[1])
print(pairs, dict(pairs), {v: k for k, v in pairs})
s = set()
s |= {1, 2}
s.update([3], (4,), {5: None})
print(s, len(s), {1, 2} | {2, 3} & {3, 4}, ({1} | {2}) & {2})
t = (1, [2, 3])
t[1].append(4)
print(t)
try:
    hash(t)
except TypeError as e:
    print("TypeError:", e)
words = "one two three two one two".split()
unique = list(dict.fromkeys(words))
print(unique, sorted(set(words), key=words.index), max(set(words), key=words.count))
inventory = {"apples": 5, "pears": 0, "plums": 3}
in_stock = {k: v for k, v in inventory.items() if v}
print(in_stock, sorted(inventory, key=inventory.get), list(inventory.keys() - {"pears"}) == ["apples", "plums"] or True)
first, second, *rest = sorted(inventory.values(), reverse=True)
print(first, second, rest)
print([1, 2, 3][::-1][1:], "abcdef"[1::2], (0, 1, 2, 3)[-2:], list(range(10))[2:8:3], [][:5])
nested = {"a": [1, {"b": (2, 3)}]}
print(nested["a"][1]["b"][1], len(nested["a"]), str(nested))
