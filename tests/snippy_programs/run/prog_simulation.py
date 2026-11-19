# A small simulation: a queue at a counter served over discrete time steps, with a random
# generator of its own, statistics, a histogram and a report formatted as a table.
class Lcg:
    def __init__(self, seed):
        self.state = seed
    def random(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) % 2 ** 64
        return (self.state >> 11) / 2 ** 53
    def randint(self, lo, hi):
        return lo + int(self.random() * (hi - lo + 1))
class Customer:
    count = 0
    def __init__(self, arrived, needs):
        Customer.count += 1
        self.id = Customer.count
        self.arrived = arrived
        self.needs = needs
        self.started = None
    def __repr__(self):
        return f"C{self.id}"
def simulate(steps, seed):
    rng = Lcg(seed)
    waiting, served, serving, left = [], [], None, 0
    for t in range(steps):
        if rng.random() < 0.4:
            waiting.append(Customer(t, rng.randint(1, 4)))
        if serving and t - serving.started >= serving.needs:
            served.append(serving)
            serving = None
        if serving is None and waiting:
            serving = waiting.pop(0)
            serving.started = t
        if len(waiting) > 5:
            waiting.pop()
            left += 1
    return served, waiting, left
served, waiting, left = simulate(60, 2024)
waits = [c.started - c.arrived for c in served]
print(len(served), len(waiting), left, served[:5], waiting)
print(f"mean wait {sum(waits) / len(waits):.3f}, longest {max(waits)}, shortest {min(waits)}")
histogram = {}
for w in waits:
    histogram[w] = histogram.get(w, 0) + 1
for w in sorted(histogram):
    print(f"{w:>3} | {'*' * histogram[w]:<12}| {histogram[w]}")
for seed in (1, 2, 3):
    s, w, l = simulate(100, seed)
    print(f"seed {seed}: served {len(s):>3}, waiting {len(w)}, left {l:>2}, mean {sum(c.started - c.arrived for c in s) / max(1, len(s)):.2f}")
rng = Lcg(7)
samples = [rng.random() for _ in range(1000)]
mean = sum(samples) / len(samples)
var = sum((x - mean) ** 2 for x in samples) / len(samples)
buckets = [0] * 10
for x in samples:
    buckets[int(x * 10)] += 1
print(f"{mean:.4f} {var:.4f} {buckets} {min(samples):.5f} {max(samples):.5f}")
