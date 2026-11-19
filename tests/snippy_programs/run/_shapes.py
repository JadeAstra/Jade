"""Shapes, a module the import test imports."""
print("importing _shapes as", __name__)
PI = 3.14159
_private = "hidden"
__all__ = ["Circle", "area_of", "PI"]
class Circle:
    def __init__(self, r):
        self.r = r
    def area(self):
        return round(PI * self.r ** 2, 2)
def area_of(shape):
    return shape.area()
counter = 0
def bump():
    global counter
    counter += 1
    return counter
if __name__ == "__main__":
    print("never: imported, not run")
