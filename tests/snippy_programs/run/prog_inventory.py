# An inventory report: nested dicts, sorting by several keys, grouping, formatted columns with
# thousands separators and percentages, totals and a bar chart.
stock = {
    "widget": {"category": "tools", "price": 2.5, "qty": 1200, "sold": 340},
    "gadget": {"category": "tools", "price": 12.99, "qty": 150, "sold": 120},
    "doohickey": {"category": "parts", "price": 0.35, "qty": 25000, "sold": 18000},
    "thingamajig": {"category": "parts", "price": 7.0, "qty": 0, "sold": 75},
    "whatsit": {"category": "misc", "price": 99.95, "qty": 12, "sold": 3},
}
header = f"{'item':<12}{'cat':<7}{'price':>9}{'qty':>9}{'sold':>8}{'value':>12}{'sell%':>8}"
print(header)
print("-" * len(header))
grand = 0.0
for name, s in sorted(stock.items(), key=lambda kv: (kv[1]["category"], -kv[1]["price"])):
    value = s["price"] * s["qty"]
    grand += value
    rate = s["sold"] / (s["sold"] + s["qty"]) if s["sold"] + s["qty"] else 0
    print(f"{name:<12}{s['category']:<7}{s['price']:>9.2f}{s['qty']:>9,}{s['sold']:>8,}{value:>12,.2f}{rate:>8.1%}")
print("-" * len(header))
print(f"{'total':<12}{'':<7}{'':>9}{sum(s['qty'] for s in stock.values()):>9,}{sum(s['sold'] for s in stock.values()):>8,}{grand:>12,.2f}")
by_cat = {}
for name, s in stock.items():
    by_cat.setdefault(s["category"], []).append(name)
for cat, names in sorted(by_cat.items()):
    revenue = sum(stock[n]["price"] * stock[n]["sold"] for n in names)
    print(f"{cat:>6}: {', '.join(sorted(names)):<30} revenue {revenue:>10,.2f}")
out_of_stock = [n for n, s in stock.items() if s["qty"] == 0]
low = sorted((n for n, s in stock.items() if 0 < s["qty"] < 200), key=lambda n: stock[n]["qty"])
print("out:", out_of_stock, "low:", low)
top = max(stock, key=lambda n: stock[n]["price"] * stock[n]["sold"])
print("best seller by revenue:", top, f"{stock[top]['price'] * stock[top]['sold']:,.2f}")
scale = max(s["sold"] for s in stock.values())
for name, s in stock.items():
    bar = "#" * round(30 * s["sold"] / scale)
    print(f"{name:>12} {bar:<30} {s['sold']:>6}")
print(f"{0.5:.0%} {1/3:.3%} {1234.5:+,.1f} {-0.0001:.2e} {42:#06x} {3.0:g} {1e-10:g}")
