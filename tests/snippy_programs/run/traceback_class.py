# An error nothing catches, raised while a class body runs, from a method its decorator calls:
# the traceback names the class body's frame by the class's name.
def check(fn):
    fn.checked = validate(fn.__name__)
    return fn
def validate(name):
    if name.startswith("bad"):
        raise TypeError(f"{name} is not allowed")
    return True
class Service:
    @check
    def good(self):
        return 1
    print("good checked:", good.checked)
    @check
    def bad_method(self):
        return 2
print("never reached")
