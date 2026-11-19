# f-strings: conversions, specs, nested fields, = and expressions.
name = "World"
n = 42
pi = 3.14159265
items = [1, 2, 3]
print(f"Hello, {name}!", f"{n}", f"{n:5}|", f"{n:<5}|", f"{n:^7}|", f"{n:*>6}", f"{n:05}", f"{n:+}", f"{-n:+}", f"{n:x}", f"{n:#x}", f"{n:#o}", f"{n:b}")
print(f"{pi:.2f}", f"{pi:10.3f}|", f"{pi:e}", f"{pi:.3e}", f"{pi:g}", f"{1e20:g}", f"{0.00001:g}", f"{pi:%}", f"{0.25:.1%}", f"{1234567.891:,.2f}", f"{1234567:_}")
print(f"{name!r}", f"{name!s}", f"{name!a}", f"{'é'!a}", f"{items}", f"{items!r}", f"{len(items) * 2}", f"{items[0] + items[-1]}")
print(f"{n=}", f"{pi=:.2f}", f"{name = }", f"{1 + 1=}")
width = 8
prec = 3
print(f"{pi:{width}.{prec}f}|", f"{name:>{width}}|", f"{'nested':{'^'}{width + 2}}|")
print(f"{{braces}}", f"{{{n}}}", f"a{'b'}c", f"{'x' if n > 10 else 'y'}", f"{ {'k': 1}['k'] }")
d = {"a": 1}
print(f"{d['a']}", f'{d["a"]}', f"{'quoted'}", f"""triple {name}""", f"{n!r:>6}")
print(f"{3.0}", f"{1e16}", f"{0.1 + 0.2}", f"{True}", f"{None}", f"{-0.0}", f"{float('inf')}", f"{2**70}")
print(f"{12345.6789:.0f}", f"{0.5:.0f}", f"{1.5:.0f}", f"{2.5:.0f}", f"{-1.25:.1f}", f"{1.005:.2f}", f"{123.456:010.2f}", f"{-12:06}")
print(f"{'abc':.2}", f"{'abc':5.1}|", f"{65:c}", f"{1e-7}", f"{123456789:e}", f"{0.0:e}", f"{1:.3g}", f"{1.0:.3g}")
print(f"{10:#b}", f"{-10:b}", f"{255:08b}", f"{3.0:g}", f"{3.5:#g}", f"{1e100:.2e}", f"{1.0:n}", f"{12.0:.10g}", f"{1/3:.17g}")
