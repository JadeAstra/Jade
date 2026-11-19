# Generators: yield, next and StopIteration, a return value, send, throw, close, finally,
# yield from, infinite ones, and generators in pipelines.
def count_up(n):
    i = 0
    while i < n:
        yield i
        i += 1
print(list(count_up(5)), sum(count_up(101)), [x * 2 for x in count_up(3)])
g = count_up(2)
print(next(g), next(g))
try:
    next(g)
except StopIteration:
    print("StopIteration")
print(next(g, "default"))
def with_return():
    yield 1
    return "done"
gr = with_return()
print(next(gr))
try:
    next(gr)
except StopIteration as e:
    print("returned", e.value)
def echo():
    received = []
    while True:
        value = yield len(received)
        if value is None:
            break
        received.append(value)
    return received
e = echo()
print(next(e), e.send("a"), e.send("b"))
try:
    e.send(None)
except StopIteration as stop:
    print("final", stop.value)
def careful():
    try:
        yield 1
        yield 2
    except ValueError as err:
        print("caught inside", err)
        yield "recovered"
    finally:
        print("generator cleanup")
c = careful()
print(next(c), c.throw(ValueError("thrown in")))
c.close()
c2 = careful()
print(next(c2))
c2.close()
print("closed")
def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b
f = fib()
print([next(f) for _ in range(15)])
def take(n, it):
    for i, x in enumerate(it):
        if i >= n:
            return
        yield x
print(list(take(5, fib())), list(take(3, "abcdef")))
def inner():
    x = yield "first"
    yield f"got {x}"
    return "inner result"
def outer():
    result = yield from inner()
    yield result
    yield from range(2)
o = outer()
print(next(o), o.send("sent"), next(o), next(o), next(o))
def flatten(items):
    for item in items:
        if isinstance(item, list):
            yield from flatten(item)
        else:
            yield item
print(list(flatten([1, [2, [3, [4]], 5], [[6]]])))
def evens(it):
    return (x for x in it if x % 2 == 0)
def squares(it):
    for x in it:
        yield x * x
print(list(squares(evens(range(10)))))
gen = (i for i in range(3))
print(iter(gen) is gen, type(gen).__name__, type(count_up(1)).__name__)
def stopper():
    yield 1
    raise StopIteration
try:
    list(stopper())
except RuntimeError as err:
    print("RuntimeError:", err)
def state():
    total = 0
    while True:
        n = yield total
        total += n
s = state()
next(s)
for n in [1, 2, 3]:
    print(s.send(n))
def gen_finally():
    try:
        yield 1
    finally:
        print("finally on exhaustion")
for x in gen_finally():
    print("got", x)
def interleave(*its):
    iters = [iter(i) for i in its]
    while iters:
        for it in list(iters):
            try:
                yield next(it)
            except StopIteration:
                iters.remove(it)
print(list(interleave("abc", [1, 2], "XYZW")))
