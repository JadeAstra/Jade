# Graphs: breadth- and depth-first search, shortest paths, topological order, connected parts.
edges = [("a", "b"), ("a", "c"), ("b", "d"), ("c", "d"), ("d", "e"), ("f", "g")]
graph = {}
for x, y in edges:
    graph.setdefault(x, []).append(y)
    graph.setdefault(y, []).append(x)
print({k: sorted(v) for k, v in sorted(graph.items())})
def bfs(start):
    seen, order, frontier = {start}, [], [start]
    while frontier:
        node = frontier.pop(0)
        order.append(node)
        for n in sorted(graph[node]):
            if n not in seen:
                seen.add(n)
                frontier.append(n)
    return order
def dfs(node, seen=None):
    seen = set() if seen is None else seen
    seen.add(node)
    out = [node]
    for n in sorted(graph[node]):
        if n not in seen:
            out += dfs(n, seen)
    return out
print(bfs("a"), dfs("a"), bfs("f"))
def shortest(start, goal):
    previous = {start: None}
    queue = [start]
    while queue:
        node = queue.pop(0)
        if node == goal:
            path = []
            while node is not None:
                path.append(node)
                node = previous[node]
            return path[::-1]
        for n in sorted(graph[node]):
            if n not in previous:
                previous[n] = node
                queue.append(n)
    return None
print(shortest("a", "e"), shortest("a", "g"))
def components():
    seen, parts = set(), []
    for node in sorted(graph):
        if node not in seen:
            part = dfs(node)
            seen.update(part)
            parts.append(sorted(part))
    return parts
print(components())
weighted = {"A": {"B": 4, "C": 2}, "B": {"D": 5}, "C": {"B": 1, "D": 8}, "D": {}}
def dijkstra(src):
    dist = {n: float("inf") for n in weighted}
    dist[src] = 0
    unvisited = set(weighted)
    while unvisited:
        u = min(unvisited, key=lambda n: (dist[n], n))
        unvisited.remove(u)
        for v, w in weighted[u].items():
            if dist[u] + w < dist[v]:
                dist[v] = dist[u] + w
    return dist
print(dijkstra("A"))
deps = {"app": ["lib", "utils"], "lib": ["core"], "utils": ["core"], "core": []}
def topo(deps):
    order, visiting, done = [], set(), set()
    def visit(n):
        if n in done:
            return
        if n in visiting:
            raise ValueError(f"cycle at {n}")
        visiting.add(n)
        for d in deps[n]:
            visit(d)
        visiting.discard(n)
        done.add(n)
        order.append(n)
    for n in sorted(deps):
        visit(n)
    return order
print(topo(deps))
try:
    topo({"x": ["y"], "y": ["x"]})
except ValueError as e:
    print("ValueError:", e)
maze = ["S.#....",
        ".##.##.",
        "...#...",
        ".#...#E"]
start = next((r, c) for r, row in enumerate(maze) for c, ch in enumerate(row) if ch == "S")
goal = next((r, c) for r, row in enumerate(maze) for c, ch in enumerate(row) if ch == "E")
dist = {start: 0}
frontier = [start]
while frontier:
    r, c = frontier.pop(0)
    for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nr, nc = r + dr, c + dc
        if 0 <= nr < len(maze) and 0 <= nc < len(maze[0]) and maze[nr][nc] != "#" and (nr, nc) not in dist:
            dist[(nr, nc)] = dist[(r, c)] + 1
            frontier.append((nr, nc))
print(start, goal, dist.get(goal), len(dist))
