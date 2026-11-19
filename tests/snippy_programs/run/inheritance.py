# Inheritance: overriding, super(), multiple inheritance and the MRO, isinstance, abstract-ish
# bases, classes made from builtins.
class Animal:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "..."
    def describe(self):
        return f"{self.name} says {self.speak()}"
class Dog(Animal):
    def speak(self):
        return "woof"
class Puppy(Dog):
    def __init__(self, name, toy):
        super().__init__(name)
        self.toy = toy
    def speak(self):
        return super().speak() + "!"
    def describe(self):
        return super().describe() + f" and plays with {self.toy}"
p = Puppy("Bit", "ball")
print(p.describe(), Dog("Rex").describe(), Animal("x").describe())
print(isinstance(p, Animal), isinstance(p, Dog), issubclass(Puppy, Animal), issubclass(Animal, Dog), issubclass(Dog, (int, Animal)))
print([c.__name__ for c in Puppy.__mro__], Puppy.__bases__[0].__name__)
class A:
    def who(self):
        return "A"
class B(A):
    def who(self):
        return "B" + super().who()
class C(A):
    def who(self):
        return "C" + super().who()
class D(B, C):
    def who(self):
        return "D" + super().who()
print(D().who(), [k.__name__ for k in D.mro()])
class Base:
    def __init__(self):
        self.log = ["Base"]
class Left(Base):
    def __init__(self):
        super().__init__()
        self.log.append("Left")
class Right(Base):
    def __init__(self):
        super().__init__()
        self.log.append("Right")
class Both(Left, Right):
    def __init__(self):
        super().__init__()
        self.log.append("Both")
print(Both().log)
class Shape:
    def area(self):
        raise NotImplementedError("subclasses define area")
    def __str__(self):
        return f"{type(self).__name__} with area {self.area()}"
class Square(Shape):
    def __init__(self, side):
        self.side = side
    def area(self):
        return self.side ** 2
class Circle(Shape):
    def __init__(self, r):
        self.r = r
    def area(self):
        return round(3.14159 * self.r ** 2, 2)
for s in [Square(3), Circle(1)]:
    print(s)
try:
    Shape().area()
except NotImplementedError as e:
    print("NotImplementedError:", e)
class MyList(list):
    def total(self):
        return sum(self)
ml = MyList([1, 2, 3])
ml.append(4)
print(ml, ml.total(), len(ml), ml[0], isinstance(ml, list), type(ml).__name__, ml + [5])
class MyDict(dict):
    def __missing__(self, key):
        return f"no {key}"
md = MyDict(a=1)
print(md["a"], md["b"], len(md), md)
class Stringy(str):
    def shout(self):
        return self.upper() + "!"
s = Stringy("hi")
print(s, s.shout(), s + " there", len(s), isinstance(s, str))
class Pos(int):
    def __new__(cls, value):
        return super().__new__(cls, abs(value))
print(Pos(-5), Pos(-5) + 1, isinstance(Pos(3), int))
class Cls:
    @classmethod
    def create(cls):
        return cls.__name__
class Sub(Cls):
    pass
print(Cls.create(), Sub.create())
class WithInit:
    def __init__(self, x):
        self.x = x
class Inherits(WithInit):
    pass
print(Inherits(5).x)
