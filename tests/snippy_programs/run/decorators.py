# Decorators: of functions, with arguments, stacked, on classes and methods.
def shout(func):
    def wrapper(*args, **kwargs):
        return func(*args, **kwargs).upper()
    wrapper.__name__ = func.__name__
    return wrapper
@shout
def hello(name):
    return f"hello {name}"
print(hello("ann"), hello.__name__)
def repeat(times):
    def decorate(func):
        def wrapper(*args):
            return [func(*args) for _ in range(times)]
        return wrapper
    return decorate
@repeat(3)
def roll(x):
    return x * 2
print(roll(5))
calls = []
def trace(label):
    def decorate(func):
        def wrapper(*a):
            calls.append((label, func.__name__, a))
            return func(*a)
        return wrapper
    return decorate
@trace("outer")
@trace("inner")
def add(a, b):
    return a + b
print(add(1, 2), calls)
def register(cls):
    cls.registered = True
    return cls
@register
class Thing:
    pass
print(Thing.registered)
def memoize(func):
    cache = {}
    def wrapper(n):
        if n not in cache:
            cache[n] = func(n)
        return cache[n]
    wrapper.cache = cache
    return wrapper
@memoize
def slow_square(n):
    print("computing", n)
    return n * n
print(slow_square(4), slow_square(4), slow_square(5), sorted(slow_square.cache.items()))
class Greeter:
    def __init__(self, name):
        self.name = name
    @shout
    def greet(self):
        return "hi " + self.name
    @staticmethod
    def static_one():
        return "static"
    @classmethod
    def make(cls, name):
        return cls(name)
print(Greeter("bo").greet(), Greeter.static_one(), Greeter.make("al").name, type(Greeter.make("x")).__name__)
order = []
def first(f):
    order.append("first applied")
    return f
def second(f):
    order.append("second applied")
    return f
@first
@second
def f():
    pass
print(order)
def deco_with_state(func):
    func.calls = 0
    def wrapper():
        func.calls += 1
        return func.calls
    return wrapper
@deco_with_state
def tick():
    pass
print(tick(), tick(), tick())
