# An error nothing catches, raised inside a method called from a generator called from a lambda
# inside a function: its traceback names each of this file's lines and functions as Python's.
class Inventory:
    def __init__(self):
        self.items = {"apple": 3}
    def take(self, name):
        count = self.items[name]
        return count
def names(inv, wanted):
    for w in wanted:
        yield inv.take(w)
def report(inv):
    collect = lambda ws: list(names(inv, ws))
    return collect(["apple", "pear"])
print("before")
print(report(Inventory()))
