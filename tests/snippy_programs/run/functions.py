# Functions: every kind of parameter, defaults made once, keyword calls, * and ** in calls,
# and the errors of a call that does not fit.
def greet(name, greeting="Hello"):
    return f"{greeting}, {name}!"
print(greet("Ann"), greet("Bob", "Hi"), greet(greeting="Hey", name="Cy"), greet(*["Di"]), greet(**{"name": "Ed"}))
def total(*args, **kwargs):
    return args, kwargs
print(total(), total(1, 2), total(a=1), total(1, b=2), total(*[1, 2], *(3,), **{"x": 1}, **{"y": 2}))
def kwonly(a, *, b, c=3):
    return a, b, c
print(kwonly(1, b=2), kwonly(1, c=4, b=5))
def posonly(a, b, /, c, *, d):
    return a, b, c, d
print(posonly(1, 2, 3, d=4), posonly(1, 2, c=3, d=4))
def everything(a, b=2, /, c=3, *args, d, e=5, **kw):
    return a, b, c, args, d, e, kw
print(everything(1, d=4), everything(1, 2, 3, 4, 5, d=6, e=7, f=8))
def append_to(item, target=[]):
    target.append(item)
    return target
print(append_to(1), append_to(2), append_to(3, []))
def counter(start=0, step=1):
    return start + step
print(counter(), counter(5), counter(step=10), counter(1, step=2))
def no_return():
    pass
def early(x):
    if x:
        return "early"
    print("late path")
print(no_return(), early(True), early(False))
def many(a, b, c, d, e, f, g, h):
    return a + b + c + d + e + f + g + h
print(many(1, 2, 3, 4, 5, 6, 7, 8), many(*range(8)))
def recursive_sum(items):
    return 0 if not items else items[0] + recursive_sum(items[1:])
print(recursive_sum(list(range(50))))
def show_errors(f, *args, **kwargs):
    try:
        f(*args, **kwargs)
    except TypeError as e:
        print("TypeError:", e)
show_errors(greet)
show_errors(greet, 1, 2, 3)
show_errors(greet, "a", nope=1)
show_errors(greet, "a", name="b")
show_errors(kwonly, 1, 2)
show_errors(kwonly, 1)
show_errors(posonly, 1, b=2, c=3, d=4)
show_errors(posonly, 1, 2, 3, 4)
show_errors(lambda: 0, 1)
show_errors(lambda x, y: 0, 1)
show_errors(many, 1, 2, 3)
print(greet.__name__, greet.__defaults__, kwonly.__kwdefaults__, append_to.__defaults__)
def documented():
    "The docstring."
    return 1
print(documented.__doc__, documented.__qualname__, (lambda: 0).__name__)
def outer():
    def inner():
        return 1
    return inner
print(outer().__qualname__, outer()())
f = greet
print(f("Z"), f is greet, callable(f), callable(5))
def apply(func, *args, **kwargs):
    return func(*args, **kwargs)
print(apply(max, 3, 9, 2), apply(sorted, [3, 1, 2], reverse=True), apply(greet, "W", greeting="Yo"))
