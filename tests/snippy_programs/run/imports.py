# Importing the program's own modules: import, import as, from-import, import *, a module run
# once however often it is imported, its globals seen and changed through it, __name__ and sys.
import _shapes
import _shapes as shp
from _shapes import Circle, area_of
from _greet import greeting, used_counter
import sys
print(__name__, _shapes.__name__, _shapes is shp, _shapes.__doc__)
print(Circle(2).area(), area_of(Circle(1)), _shapes.PI, shp.Circle is Circle)
print(greeting("ann"), used_counter, _shapes.counter)
print(_shapes.bump(), _shapes.bump(), _shapes.counter)
_shapes.PI = 3
print(Circle(1).area(), _shapes.PI)
_shapes.added = "new attribute"
print(_shapes.added, hasattr(_shapes, "added"), hasattr(_shapes, "nope"))
try:
    _shapes.nope
except AttributeError as e:
    print("AttributeError:", e)
try:
    from _shapes import nothing_such
except ImportError as e:
    print("ImportError:", type(e).__name__)
try:
    import no_module_named_this
except ModuleNotFoundError as e:
    print("ModuleNotFoundError:", e)
print("_shapes" in sys.modules, type(sys.argv), sys.argv[0].endswith("imports.py"), sys.maxsize, len(sys.version_info) >= 3)
def local_import():
    import _shapes as inner
    return inner.PI
print(local_import())
from _shapes import *
print(area_of(Circle(3)), "bump" in dir(), "_private" in dir())
sys.stdout.write("written to stdout\n")
print("to stderr too", file=sys.stderr)
print("one", "two", sep="-", end="!\n", file=sys.stdout)
sys.exit(3)
