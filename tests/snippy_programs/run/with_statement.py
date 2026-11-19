# The with statement: __enter__ and __exit__, what as binds, an exception given to __exit__ and
# kept or passed on, several managers, return and break inside.
class Managed:
    def __init__(self, name, swallow=False):
        self.name = name
        self.swallow = swallow
    def __enter__(self):
        print("enter", self.name)
        return self.name.upper()
    def __exit__(self, exc_type, exc, tb):
        print("exit", self.name, exc_type.__name__ if exc_type else None, exc)
        return self.swallow
with Managed("a") as value:
    print("inside with", value)
with Managed("b", swallow=True):
    raise ValueError("swallowed")
print("after swallowed")
try:
    with Managed("c"):
        raise KeyError("passed on")
except KeyError as e:
    print("caught", repr(e))
with Managed("outer") as o, Managed("inner") as i:
    print("both", o, i)
def returns_inside():
    with Managed("ret"):
        return "returned"
print(returns_inside())
for n in range(3):
    with Managed(f"loop{n}"):
        if n == 1:
            break
class Timer:
    def __enter__(self):
        self.entered = True
        return self
    def __exit__(self, *args):
        self.exited = True
with Timer() as t:
    pass
print(t.entered, t.exited)
try:
    with 5:
        pass
except TypeError as e:
    print("TypeError:", e)
class Resource:
    def __init__(self):
        self.log = []
    def __enter__(self):
        self.log.append("open")
        return self
    def __exit__(self, et, ev, tb):
        self.log.append("close")
        return False
r = Resource()
try:
    with r:
        r.log.append("work")
        1 / 0
except ZeroDivisionError:
    r.log.append("handled")
print(r.log)
with Managed("nested1"):
    with Managed("nested2", swallow=True):
        raise RuntimeError("deep")
    print("still in nested1")
