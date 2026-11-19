# Closures and scopes: nonlocal, global, late binding, UnboundLocalError, NameError.
def make_counter():
    count = 0
    def increment(by=1):
        nonlocal count
        count += by
        return count
    return increment
c1 = make_counter()
c2 = make_counter()
print(c1(), c1(), c1(5), c2())
def make_adders():
    adders = []
    for i in range(3):
        adders.append(lambda x: x + i)
    return adders
print([f(10) for f in make_adders()])
def make_adders_bound():
    return [lambda x, i=i: x + i for i in range(3)]
print([f(10) for f in make_adders_bound()])
total = 0
def add_global(n):
    global total
    total += n
add_global(5)
add_global(7)
print(total)
def define_global():
    global created_later
    created_later = "made inside"
define_global()
print(created_later)
x = "global x"
def shadow():
    x = "local x"
    def inner():
        return x
    return inner()
print(shadow(), x)
def reads_global():
    return x
print(reads_global())
def broken():
    print(y_local)
    y_local = 1
try:
    broken()
except UnboundLocalError as e:
    print("UnboundLocalError:", e)
try:
    print(not_defined_anywhere)
except NameError as e:
    print("NameError:", e)
def deep():
    a = 1
    def mid():
        b = 2
        def inner():
            nonlocal a
            a += b
            return a
        return inner
    return mid()
f = deep()
print(f(), f(), f())
def counter_obj():
    state = {"n": 0}
    def bump():
        state["n"] += 1
        return state["n"]
    return bump
b = counter_obj()
b()
print(b())
def outer_assign():
    value = 1
    def change():
        nonlocal value
        value = 2
    change()
    return value
print(outer_assign())
def free_before():
    def get():
        return later
    try:
        get()
    except NameError as e:
        print("NameError:", e)
    later = 5
    return get()
print(free_before())
def deleted():
    v = 1
    del v
    try:
        return v
    except UnboundLocalError as e:
        return str(e)
print(deleted())
gen_list = []
for n in range(3):
    def fn(k=n):
        return k * 10
    gen_list.append(fn)
print([g() for g in gen_list])
