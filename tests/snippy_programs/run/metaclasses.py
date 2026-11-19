# Metaclasses, __init_subclass__, __class_getitem__, type() making classes, __new__.
class Registry(type):
    classes = []
    def __new__(mcs, name, bases, ns):
        cls = super().__new__(mcs, name, bases, ns)
        mcs.classes.append(name)
        return cls
class Plugin(metaclass=Registry):
    pass
class Audio(Plugin):
    pass
print(Registry.classes, type(Plugin).__name__, type(Audio) is Registry)
class Base:
    subclasses = []
    def __init_subclass__(cls, tag="none", **kwargs):
        super().__init_subclass__(**kwargs)
        cls.tag = tag
        Base.subclasses.append(cls.__name__)
class One(Base, tag="first"):
    pass
class Two(Base):
    pass
print(Base.subclasses, One.tag, Two.tag)
Dynamic = type("Dynamic", (object,), {"x": 5, "double": lambda self: self.x * 2})
d = Dynamic()
print(Dynamic.__name__, d.x, d.double(), type(d).__name__)
class Singleton:
    _instance = None
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance
print(Singleton() is Singleton())
class Box:
    def __class_getitem__(cls, item):
        return f"Box of {item.__name__}"
print(Box[int], list[int], dict[str, int])
class Meta(type):
    def __call__(cls, *args):
        print("making", cls.__name__, args)
        return super().__call__(*args)
class Made(metaclass=Meta):
    def __init__(self, v):
        self.v = v
print(Made(3).v)
class Upper(type):
    def __new__(mcs, name, bases, ns):
        upper = {k.upper() if not k.startswith("__") else k: v for k, v in ns.items()}
        return super().__new__(mcs, name, bases, upper)
class Loud(metaclass=Upper):
    greeting = "hi"
print(Loud.GREETING, hasattr(Loud, "greeting"))
class Counted(type):
    count = 0
    def __init__(cls, name, bases, ns):
        super().__init__(name, bases, ns)
        Counted.count += 1
class X(metaclass=Counted):
    pass
class Y(X):
    pass
print(Counted.count)
class Point:
    def __new__(cls, x, y):
        obj = super().__new__(cls)
        obj.made_by_new = True
        return obj
    def __init__(self, x, y):
        self.x, self.y = x, y
pt = Point(1, 2)
print(pt.made_by_new, pt.x, pt.y)
