# Classes: attributes of the class and of each object, methods, class and static methods,
# what type() and __dict__ say, attributes added and removed as it runs.
class Dog:
    species = "canis"
    count = 0
    def __init__(self, name, age=1):
        self.name = name
        self.age = age
        Dog.count += 1
    def bark(self):
        return f"{self.name} says woof"
    def birthday(self):
        self.age += 1
        return self
    @classmethod
    def how_many(cls):
        return cls.count
    @staticmethod
    def kind():
        return "dog"
a = Dog("Rex")
b = Dog("Fido", 5)
print(a.bark(), b.bark(), a.age, b.age, a.birthday().birthday().age, Dog.how_many(), a.how_many(), Dog.kind(), a.kind())
print(a.species, Dog.species, type(a).__name__, type(a) is Dog, isinstance(a, Dog), a.__class__.__name__)
print(a.__dict__, sorted(vars(b)), Dog.__name__, Dog.__qualname__, Dog.__module__)
a.species = "own"
print(a.species, b.species, Dog.species)
del a.species
print(a.species)
Dog.legs = 4
print(a.legs, hasattr(a, "legs"), hasattr(a, "wings"), getattr(a, "wings", "none"))
setattr(a, "color", "brown")
print(a.color, getattr(a, "color"))
delattr(a, "color")
print(hasattr(a, "color"))
try:
    a.missing
except AttributeError as e:
    print("AttributeError:", e)
try:
    Dog.missing
except AttributeError as e:
    print("AttributeError:", e)
bound = a.bark
print(bound(), bound.__self__ is a, bound.__func__ is Dog.bark, Dog.bark(b))
class Empty:
    pass
e = Empty()
e.x = 1
print(e.x, Empty.__name__, isinstance(e, object), Empty.__bases__, Empty.__mro__)
class Point:
    __slots__ = ()
    def __init__(self):
        pass
class Temp:
    def __init__(self):
        self._c = 0
    def get(self):
        return self._c
    def set(self, v):
        self._c = v
t = Temp()
t.set(25)
print(t.get(), t._c)
class Nested:
    class Inner:
        value = 42
    def make(self):
        return self.Inner()
print(Nested.Inner.value, Nested().make().value, Nested.Inner.__qualname__)
def method_outside(self):
    return "added later " + self.name
Dog.extra = method_outside
print(a.extra())
class Counter:
    total = 0
    def __init__(self):
        self.total += 1
c = Counter()
print(c.total, Counter.total)
print(type(Dog), type(type), isinstance(Dog, type), type(3).__name__, type("x") is str)
