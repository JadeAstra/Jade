# Exceptions: try/except/else/finally, raising, classes of a program's own, their args, str and
# repr, catching by class and tuple, bare raise, except ... as, nested handlers.
class AppError(Exception):
    pass
class NotFound(AppError):
    def __init__(self, what, code=404):
        super().__init__(f"{what} not found")
        self.what = what
        self.code = code
def lookup(key):
    if key == "missing":
        raise NotFound(key)
    if key == "bad":
        raise ValueError("bad key", 42)
    return key.upper()
for key in ["ok", "missing", "bad"]:
    try:
        result = lookup(key)
    except NotFound as e:
        print("NotFound:", e, e.what, e.code, e.args, isinstance(e, AppError), isinstance(e, Exception))
    except (TypeError, ValueError) as e:
        print("ValueError/TypeError:", e, e.args, repr(e))
    else:
        print("ok:", result)
    finally:
        print("finally for", key)
try:
    raise AppError
except AppError as e:
    print(repr(e), str(e) == "", e.args)
try:
    raise Exception("one", 2, [3])
except Exception as e:
    print(str(e), repr(e))
def reraise():
    try:
        1 / 0
    except ZeroDivisionError:
        print("logging, then raising again")
        raise
try:
    reraise()
except ZeroDivisionError as e:
    print("outer caught:", e)
try:
    try:
        raise KeyError("k")
    except KeyError as inner:
        print("inner:", repr(inner))
        raise
except LookupError as outer:
    print("outer:", type(outer).__name__, outer)
e_kept = None
try:
    raise RuntimeError("kept")
except RuntimeError as e:
    e_kept = e
print(e_kept, type(e_kept).__name__)
try:
    e
except NameError as ne:
    print("NameError:", ne)
def finally_returns():
    try:
        return "from try"
    finally:
        print("finally runs before the return")
print(finally_returns())
def swallow():
    for i in range(3):
        try:
            if i == 1:
                raise ValueError
        except ValueError:
            continue
        finally:
            print("finally", i)
        print("body", i)
swallow()
try:
    try:
        raise TypeError("first")
    finally:
        print("cleanup")
except TypeError as e:
    print("after cleanup:", e)
try:
    raise ValueError("x") from None
except ValueError as e:
    print(e.__cause__, e.__suppress_context__)
class WithStr(Exception):
    def __str__(self):
        return "custom message"
try:
    raise WithStr(1, 2)
except WithStr as e:
    print(e, repr(e), e.args)
try:
    assert 1 + 1 == 3, "math is broken"
except AssertionError as e:
    print("AssertionError:", e)
try:
    assert False
except AssertionError as e:
    print("AssertionError:", repr(e))
def check(n):
    if not isinstance(n, int):
        raise TypeError(f"expected int, got {type(n).__name__}")
    return n
for v in [1, "two", 3.0]:
    try:
        print(check(v))
    except TypeError as e:
        print(e)
try:
    raise ExceptionGroup("several", [ValueError(1), TypeError(2)])
except ExceptionGroup as eg:
    print(eg.message, len(eg.exceptions), [type(x).__name__ for x in eg.exceptions])
err = ValueError("note me")
err.add_note("first note")
print(err.__notes__, BaseException.__subclasses__ is not None)
try:
    raise SystemExit(3)
except SystemExit as e:
    print("SystemExit code", e.code)
try:
    raise StopIteration(5)
except StopIteration as e:
    print("StopIteration value", e.value)
