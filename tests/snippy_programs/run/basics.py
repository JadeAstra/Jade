# Printing, numbers and names: the simplest programs.
print("hello, world")
print(1, 2, 3)
print(1, 2, 3, sep=", ")
print("no newline", end="|")
print("after")
print()
print("a", "b", sep="")
x = 10
y = 3
print(x + y, x - y, x * y, x / y, x // y, x % y, x ** y)
print(-x // y, -x % y, x // -y, x % -y)
name = "Snippy"
print("Hi", name, "!", len(name))
print(type(1), type(1.0), type("a"), type(None), type(True), type([]), type(()), type({}), type(set()))
a = b = c = 5
print(a, b, c)
a, b = 1, 2
a, b = b, a
print(a, b)
print(None, True, False, ...)
print(0.1 + 0.2, 1 / 3, 2 / 2, 1e16, 1e-5, 123456789.123456789)
print(10 ** 20, 2 ** 100, -(2 ** 63), 2 ** 63 - 1)
