# Records written by hand as Python users write them before dataclasses: __eq__, __hash__,
# __repr__, ordering, copying, grouping and summarising.
class Employee:
    __slots__ = ("name", "dept", "salary", "age")
    def __init__(self, name, dept, salary, age):
        self.name, self.dept, self.salary, self.age = name, dept, salary, age
    def __repr__(self):
        return f"Employee({self.name!r}, {self.dept!r}, {self.salary}, {self.age})"
    def key(self):
        return (self.name, self.dept, self.salary, self.age)
    def __eq__(self, other):
        return isinstance(other, Employee) and self.key() == other.key()
    def __hash__(self):
        return hash(self.key())
    def __lt__(self, other):
        return self.key() < other.key()
staff = [Employee("Ann", "eng", 120, 34), Employee("Bob", "ops", 90, 45), Employee("Cy", "eng", 110, 28),
         Employee("Di", "sales", 80, 39), Employee("Ed", "ops", 95, 31), Employee("Fay", "eng", 130, 41)]
print(sorted(staff)[0], max(staff, key=lambda e: e.salary).name, min(staff, key=lambda e: e.age).name)
by_dept = {}
for e in staff:
    by_dept.setdefault(e.dept, []).append(e)
for dept, people in sorted(by_dept.items()):
    total = sum(p.salary for p in people)
    print(f"{dept:<6} {len(people)} people, total {total:>4}, mean {total / len(people):6.2f}, names {[p.name for p in people]}")
print(Employee("Ann", "eng", 120, 34) == staff[0], staff[0] in staff, len(set(staff + [Employee("Ann", "eng", 120, 34)])))
raises = {e.name: round(e.salary * 1.05, 1) for e in staff if e.dept == "eng"}
print(raises)
older = [e.name for e in staff if e.age > 35]
print(older, all(e.salary > 50 for e in staff), any(e.dept == "hr" for e in staff))
table = [("Name", "Dept", "Salary")] + [(e.name, e.dept, str(e.salary)) for e in sorted(staff, key=lambda e: -e.salary)]
widths = [max(len(row[i]) for row in table) for i in range(3)]
for row in table:
    print("  ".join(cell.ljust(w) if i < 2 else cell.rjust(w) for i, (cell, w) in enumerate(zip(row, widths))))
try:
    staff[0].nickname = "A"
except AttributeError as e:
    print("AttributeError:", type(e).__name__)
import sys
point = {"x": 1, "y": 2}
moved = dict(point, x=5)
print(point, moved, {**point, "z": 3}, sorted(point.items(), key=lambda kv: kv[1], reverse=True))
nested = {"team": {"lead": staff[0].name, "size": len(staff)}, "tags": ["a", "b"]}
copied = {k: (dict(v) if isinstance(v, dict) else list(v)) for k, v in nested.items()}
copied["team"]["size"] = 0
copied["tags"].append("c")
print(nested, copied)
