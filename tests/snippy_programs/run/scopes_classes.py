# Scopes around classes: class bodies see globals but their methods do not see the class's
# names; __class__ and super in nested functions; nonlocal across levels; names deleted.
x = "global"
class A:
    x = "class"
    y = x + " seen in body"
    def method(self):
        return x
    def uses_class_cell(self):
        return __class__.__name__
    items = [x for _ in range(2)]
print(A.y, A().method(), A().uses_class_cell(), A.items)
def factory():
    x = "enclosing"
    class B:
        print("class body sees", x)
        z = x
        def get(self):
            return x
    return B
B = factory()
print(B.z, B().get())
def counter():
    count = 0
    def outer():
        def inner():
            nonlocal count
            count += 1
            return count
        return inner()
    return outer
c = counter()
print(c(), c(), c())
class Base:
    def hello(self):
        return "base hello"
class Child(Base):
    def hello(self):
        def helper():
            return super(Child, self).hello()
        return "child + " + helper()
    def twice(self):
        return [super().hello() for _ in range(2)]
print(Child().hello(), Child().twice())
value = 1
def read_then_global():
    global value
    value += 1
    return value
print(read_then_global(), value)
def make_class(n):
    class Dyn:
        size = n
        def describe(self):
            return f"{type(self).__name__} of size {self.size}"
    return Dyn
print(make_class(3)().describe(), make_class(5).size)
class Counter:
    instances = 0
    def __init__(self):
        Counter.instances += 1
        self.id = Counter.instances
print([Counter().id for _ in range(3)], Counter.instances)
def shadowing():
    len = 5
    return len
print(shadowing(), len("abc"))
for i in range(3):
    pass
print("loop variable after", i)
try:
    del i
    print(i)
except NameError as e:
    print("NameError:", e)
