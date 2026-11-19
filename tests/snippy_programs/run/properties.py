# property, its setter and deleter, descriptors of a program's own, __set_name__,
# __getattr__, __setattr__, __getattribute__, __delattr__.
class Celsius:
    def __init__(self, degrees=0):
        self._degrees = degrees
    @property
    def degrees(self):
        "The temperature."
        return self._degrees
    @degrees.setter
    def degrees(self, value):
        if value < -273.15:
            raise ValueError("below absolute zero")
        self._degrees = value
    @degrees.deleter
    def degrees(self):
        print("deleting")
        del self._degrees
    @property
    def fahrenheit(self):
        return self._degrees * 9 / 5 + 32
c = Celsius(25)
print(c.degrees, c.fahrenheit)
c.degrees = 100
print(c.degrees, c.fahrenheit, Celsius.degrees.__doc__)
try:
    c.degrees = -300
except ValueError as e:
    print("ValueError:", e)
try:
    c.fahrenheit = 5
except AttributeError as e:
    print("AttributeError:", e)
del c.degrees
print(hasattr(c, "_degrees"))
class Typed:
    def __init__(self, kind):
        self.kind = kind
    def __set_name__(self, owner, name):
        self.name = "_" + name
    def __get__(self, obj, owner):
        if obj is None:
            return self
        return getattr(obj, self.name, None)
    def __set__(self, obj, value):
        if not isinstance(value, self.kind):
            raise TypeError(f"{self.name[1:]} must be {self.kind.__name__}")
        setattr(obj, self.name, value)
class Person:
    name = Typed(str)
    age = Typed(int)
    def __init__(self, name, age):
        self.name = name
        self.age = age
p = Person("Ann", 30)
print(p.name, p.age, p.__dict__, type(Person.name).__name__)
try:
    p.age = "old"
except TypeError as e:
    print("TypeError:", e)
class Lazy:
    def __getattr__(self, name):
        return f"computed {name}"
lz = Lazy()
lz.real = 1
print(lz.real, lz.anything, getattr(lz, "other"))
class Tracked:
    def __init__(self):
        self.__dict__["log"] = []
    def __setattr__(self, name, value):
        self.log.append(name)
        super().__setattr__(name, value)
    def __delattr__(self, name):
        self.log.append("del " + name)
        super().__delattr__(name)
t = Tracked()
t.a = 1
t.b = 2
del t.a
print(t.log, t.b, hasattr(t, "a"))
class Shout:
    def __init__(self):
        self.word = "hi"
    def __getattribute__(self, name):
        value = super().__getattribute__(name)
        return value.upper() if isinstance(value, str) else value
print(Shout().word)
class Const:
    @property
    def pi(self):
        return 3.14
print(Const().pi, isinstance(Const.pi, property))
class Account:
    def __init__(self):
        self._balance = 0
    balance = property(lambda self: self._balance, None, None, "the balance")
    def deposit(self, n):
        self._balance += n
acc = Account()
acc.deposit(10)
print(acc.balance, Account.balance.__doc__)
class Base:
    @property
    def value(self):
        return 1
class Derived(Base):
    @property
    def value(self):
        return super().value + 1
print(Derived().value)
