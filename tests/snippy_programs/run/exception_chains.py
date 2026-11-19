# Exception chaining: raise ... from, the context Python keeps, and how an uncaught one is shown:
# this program ends with one, its traceback on standard error naming this file's lines.
def parse(text):
    try:
        return int(text)
    except ValueError as e:
        raise RuntimeError(f"cannot parse {text!r}") from e
try:
    parse("abc")
except RuntimeError as e:
    print(e, "| cause:", repr(e.__cause__), "| context is cause:", e.__context__ is e.__cause__)
def implicit():
    try:
        {}["k"]
    except KeyError:
        raise ValueError("while handling")
try:
    implicit()
except ValueError as e:
    print(e, "| context:", repr(e.__context__), "| cause:", e.__cause__, e.__suppress_context__)
try:
    try:
        raise TypeError("a")
    except TypeError:
        raise ValueError("b") from None
except ValueError as e:
    print(repr(e.__context__), e.__suppress_context__)
def level3(n):
    return 10 / n
def level2(n):
    return level3(n) + 1
def level1(n):
    return level2(n - 1)
print(level1(3))
def outer():
    try:
        level1(1)
    except ZeroDivisionError as e:
        raise LookupError("wrapped") from e
outer()
print("never printed")
