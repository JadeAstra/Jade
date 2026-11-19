import _shapes
from _shapes import Circle as C
def greeting(name):
    return f"hello {name}, unit circle has area {C(1).area()}"
used_counter = _shapes.bump()
