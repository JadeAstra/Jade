# if/elif/else, while and for with else, break and continue, pass, nested loops.
for n in [-5, 0, 3, 15, 100]:
    if n < 0:
        print(n, "negative")
    elif n == 0:
        print(n, "zero")
    elif n < 10:
        print(n, "small")
    else:
        print(n, "big")
i = 0
while i < 5:
    i += 1
    if i == 2:
        continue
    if i == 4:
        break
    print("while", i)
else:
    print("while ended without break")
i = 0
while i < 3:
    i += 1
else:
    print("while else ran", i)
for x in range(3):
    pass
else:
    print("for else ran", x)
for x in range(10):
    if x == 3:
        print("found", x)
        break
else:
    print("not found")
for prime_candidate in range(2, 30):
    for d in range(2, prime_candidate):
        if prime_candidate % d == 0:
            break
    else:
        print("prime", prime_candidate)
outer_done = False
for a in range(3):
    for b in range(3):
        if a * b == 2:
            outer_done = True
            break
        print(a, b)
    if outer_done:
        break
count = 0
while True:
    count += 1
    if count > 100:
        break
print("count", count)
total = 0
for i in range(1, 11):
    if i % 2:
        continue
    total += i
print("even total", total)
x = 7
result = "odd" if x % 2 else "even"
print(result, "big" if x > 5 else "small" if x > 2 else "tiny")
if not []:
    print("empty is falsy")
if 0 < x < 10:
    print("between")
for i, ch in enumerate("abc", start=1):
    print(i, ch)
for a, b in zip([1, 2, 3], "xyz"):
    print(a, b)
n = 0
while n < 3:
    n += 1
    for k in range(5):
        if k == n:
            break
    else:
        print("inner else", n)
    print("outer", n)
