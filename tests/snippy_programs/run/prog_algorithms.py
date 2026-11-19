# Classic algorithms written as a Python user writes them.
def sieve(n):
    is_prime = [True] * (n + 1)
    is_prime[0] = is_prime[1] = False
    for i in range(2, int(n ** 0.5) + 1):
        if is_prime[i]:
            for j in range(i * i, n + 1, i):
                is_prime[j] = False
    return [i for i, p in enumerate(is_prime) if p]
primes = sieve(200)
print(len(primes), primes[:15], primes[-5:], sum(primes))
def gcd(a, b):
    while b:
        a, b = b, a % b
    return a
def lcm(a, b):
    return a * b // gcd(a, b)
print(gcd(1071, 462), lcm(21, 6), gcd(2 ** 100, 6 ** 50))
def bubble_sort(items):
    items = list(items)
    n = len(items)
    for i in range(n):
        swapped = False
        for j in range(n - 1 - i):
            if items[j] > items[j + 1]:
                items[j], items[j + 1] = items[j + 1], items[j]
                swapped = True
        if not swapped:
            break
    return items
def quick_sort(items):
    if len(items) <= 1:
        return items
    pivot, *rest = items
    return quick_sort([x for x in rest if x < pivot]) + [pivot] + quick_sort([x for x in rest if x >= pivot])
def merge_sort(items):
    if len(items) <= 1:
        return items
    mid = len(items) // 2
    left, right = merge_sort(items[:mid]), merge_sort(items[mid:])
    merged = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            merged.append(left[i])
            i += 1
        else:
            merged.append(right[j])
            j += 1
    merged.extend(left[i:])
    merged.extend(right[j:])
    return merged
data = [(i * 37 + 11) % 53 for i in range(40)]
print(bubble_sort(data) == sorted(data), quick_sort(data) == sorted(data), merge_sort(data)[:10])
def binary_search(items, target):
    lo, hi = 0, len(items) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if items[mid] == target:
            return mid
        if items[mid] < target:
            lo = mid + 1
        else:
            hi = mid - 1
    return -1
s = sorted(set(data))
print([binary_search(s, t) for t in (0, 11, 52, 100)])
def collatz(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps
print(max(range(1, 1000), key=collatz), collatz(27))
def to_roman(n):
    pairs = [(1000, "M"), (900, "CM"), (500, "D"), (400, "CD"), (100, "C"), (90, "XC"), (50, "L"), (40, "XL"), (10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")]
    out = []
    for value, letters in pairs:
        while n >= value:
            out.append(letters)
            n -= value
    return "".join(out)
print([to_roman(n) for n in (1, 4, 9, 14, 40, 90, 400, 1994, 2024, 3999)])
def pascal(rows):
    row = [1]
    for _ in range(rows):
        yield row
        row = [a + b for a, b in zip([0] + row, row + [0])]
for r in pascal(6):
    print(" ".join(map(str, r)).center(20))
def knapsack(items, capacity):
    best = [0] * (capacity + 1)
    for weight, value in items:
        for c in range(capacity, weight - 1, -1):
            best[c] = max(best[c], best[c - weight] + value)
    return best[capacity]
print(knapsack([(2, 3), (3, 4), (4, 5), (5, 6)], 5))
def lcs(a, b):
    table = [[0] * (len(b) + 1) for _ in range(len(a) + 1)]
    for i, x in enumerate(a, 1):
        for j, y in enumerate(b, 1):
            table[i][j] = table[i - 1][j - 1] + 1 if x == y else max(table[i - 1][j], table[i][j - 1])
    return table[-1][-1]
print(lcs("AGGTAB", "GXTXAYB"), lcs("kitten", "sitting"))
print(2 ** 256 % 1000000007, pow(7, 2 ** 20, 10 ** 9 + 7), sum(int(d) for d in str(2 ** 1000)))
