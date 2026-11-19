# A log-processing pipeline of generators, an event system of callbacks, a state machine and a
# cache with a size limit.
LOG = """2024-01-01 10:00:01 INFO  user=ann action=login
2024-01-01 10:00:05 WARN  user=bob action=login failed=1
2024-01-01 10:01:00 INFO  user=ann action=view page=home
2024-01-01 10:02:30 ERROR user=cy  action=pay amount=12.50
2024-01-01 10:03:00 INFO  user=bob action=login
2024-01-01 10:05:00 INFO  user=ann action=logout"""
def lines(text):
    for line in text.splitlines():
        yield line.rstrip()
def parse(lines):
    for line in lines:
        date, time, level, *fields = line.split()
        record = {"time": time, "level": level}
        record.update(f.split("=", 1) for f in fields)
        yield record
def only(level, records):
    return (r for r in records if r["level"] == level)
records = list(parse(lines(LOG)))
print(len(records), [r["user"] for r in records], records[3])
print([r["time"] for r in only("INFO", records)], next(only("ERROR", records))["amount"])
levels = {}
for r in records:
    levels[r["level"]] = levels.get(r["level"], 0) + 1
print(levels, sorted({r["user"] for r in records}))
class Events:
    def __init__(self):
        self.handlers = {}
    def on(self, name):
        def register(fn):
            self.handlers.setdefault(name, []).append(fn)
            return fn
        return register
    def emit(self, name, *args):
        return [h(*args) for h in self.handlers.get(name, [])]
events = Events()
seen = []
@events.on("login")
def remember(user):
    seen.append(user)
    return f"remembered {user}"
@events.on("login")
def greet(user):
    return f"welcome {user}"
for r in records:
    if r["action"] == "login" and "failed" not in r:
        print(events.emit("login", r["user"]))
print(seen, events.emit("nothing"))
class Door:
    transitions = {("closed", "open"): "opened", ("opened", "close"): "closed", ("closed", "lock"): "locked", ("locked", "unlock"): "closed"}
    def __init__(self):
        self.state = "closed"
    def do(self, action):
        key = (self.state, action)
        if key not in self.transitions:
            raise ValueError(f"cannot {action} when {self.state}")
        self.state = self.transitions[key]
        return self.state
door = Door()
for action in ["open", "close", "lock", "open", "unlock", "open"]:
    try:
        print(action, "->", door.do(action))
    except ValueError as e:
        print("ValueError:", e)
class LRU:
    def __init__(self, size):
        self.size, self.data = size, {}
    def get(self, key):
        if key not in self.data:
            return None
        value = self.data.pop(key)
        self.data[key] = value
        return value
    def put(self, key, value):
        self.data.pop(key, None)
        self.data[key] = value
        if len(self.data) > self.size:
            oldest = next(iter(self.data))
            del self.data[oldest]
cache = LRU(3)
for k in "abcd":
    cache.put(k, k.upper())
cache.get("b")
cache.put("e", "E")
print(list(cache.data), cache.get("a"), cache.get("b"))
def running_average():
    total = count = 0
    average = None
    while True:
        value = yield average
        total += value
        count += 1
        average = total / count
avg = running_average()
next(avg)
print([avg.send(v) for v in [10, 20, 30, 40]])
