# Recursion: deep enough to need it, memoised, mutual, and too deep.
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
print([fib(n) for n in range(20)])
def fact(n):
    return 1 if n <= 1 else n * fact(n - 1)
print(fact(30), fact(50))
memo = {}
def fib_memo(n):
    if n in memo:
        return memo[n]
    r = n if n < 2 else fib_memo(n - 1) + fib_memo(n - 2)
    memo[n] = r
    return r
print(fib_memo(90), fib_memo(200))
def is_even(n):
    return True if n == 0 else is_odd(n - 1)
def is_odd(n):
    return False if n == 0 else is_even(n - 1)
print(is_even(100), is_odd(77))
def hanoi(n, a, b, c, moves):
    if n:
        hanoi(n - 1, a, c, b, moves)
        moves.append((a, c))
        hanoi(n - 1, b, a, c, moves)
    return moves
print(len(hanoi(10, "A", "B", "C", [])), hanoi(2, "A", "B", "C", []))
def ackermann(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ackermann(m - 1, 1)
    return ackermann(m - 1, ackermann(m, n - 1))
print(ackermann(2, 3))
def depth(n):
    return 0 if n == 0 else 1 + depth(n - 1)
print(depth(500))
def forever(n):
    return forever(n + 1)
try:
    forever(0)
except RecursionError as e:
    print("RecursionError caught")
def flatten(x):
    if isinstance(x, list):
        out = []
        for item in x:
            out.extend(flatten(item))
        return out
    return [x]
print(flatten([1, [2, [3, [4, [5]]]], 6]))
def permutations(items):
    if len(items) <= 1:
        return [items]
    out = []
    for i, item in enumerate(items):
        for rest in permutations(items[:i] + items[i + 1:]):
            out.append([item] + rest)
    return out
print(permutations([1, 2, 3]), len(permutations(list(range(6)))))
