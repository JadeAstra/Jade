# Exceptions meeting generators, with, finally and loops: raised inside generators and caught
# outside, thrown in, finally blocks while unwinding, exceptions in __exit__ and in handlers.
def risky_gen():
    yield 1
    raise ValueError("from generator")
try:
    for v in risky_gen():
        print("got", v)
except ValueError as e:
    print("caught", e)
def cleanup_gen():
    try:
        yield "a"
        yield "b"
    finally:
        print("generator finally")
for v in cleanup_gen():
    print(v)
    break
print("after break")
log = []
def nested():
    try:
        try:
            log.append("inner try")
            raise KeyError("k")
        finally:
            log.append("inner finally")
    except KeyError:
        log.append("outer except")
        return "returned"
    finally:
        log.append("outer finally")
print(nested(), log)
class Fragile:
    def __enter__(self):
        return self
    def __exit__(self, *a):
        raise RuntimeError("from __exit__")
try:
    with Fragile():
        raise ValueError("original")
except RuntimeError as e:
    print("RuntimeError", e, "context:", repr(e.__context__))
try:
    try:
        raise ValueError("first")
    except ValueError:
        raise TypeError("second")
except TypeError as e:
    print(repr(e), repr(e.__context__))
results = []
for i in range(5):
    try:
        if i % 2:
            raise ValueError(i)
        results.append(i)
    except ValueError as e:
        results.append(f"err{e}")
        continue
    finally:
        results.append(".")
print(results)
def gen_with_except():
    while True:
        try:
            v = yield
            print("received", v)
        except KeyError as e:
            print("handled in generator", e)
g = gen_with_except()
next(g)
g.send(1)
g.throw(KeyError("thrown"))
g.send(2)
g.close()
def count_calls():
    try:
        return "value"
    finally:
        print("finally after return value computed")
print(count_calls())
try:
    raise ValueError("a") from KeyError("b")
except ValueError as e:
    print(repr(e.__cause__), e.__suppress_context__)
def handler_raises():
    try:
        1 / 0
    except ZeroDivisionError as e:
        try:
            raise ValueError("inner") from e
        except ValueError as v:
            return type(v.__cause__).__name__
print(handler_raises())
while True:
    try:
        break
    finally:
        print("finally on break")
