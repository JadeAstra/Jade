# Conway's Game of Life on a set of live cells, and a grid printed as text.
def neighbours(cell):
    x, y = cell
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx or dy:
                yield (x + dx, y + dy)
def step(live):
    counts = {}
    for cell in live:
        for n in neighbours(cell):
            counts[n] = counts.get(n, 0) + 1
    return {cell for cell, c in counts.items() if c == 3 or (c == 2 and cell in live)}
def show(live, width=8, height=6):
    for y in range(height):
        print("".join("#" if (x, y) in live else "." for x in range(width)))
    print()
glider = {(1, 0), (2, 1), (0, 2), (1, 2), (2, 2)}
state = glider
for generation in range(4):
    print("generation", generation, len(state), sorted(state))
    show(state)
    state = step(state)
blinker = {(1, 0), (1, 1), (1, 2)}
print(step(blinker) == {(0, 1), (1, 1), (2, 1)}, step(step(blinker)) == blinker)
grid = [[0] * 5 for _ in range(5)]
for i in range(5):
    grid[i][i] = 1
    grid[i][4 - i] = 2
for row in grid:
    print(" ".join(str(v) for v in row))
transposed = [list(col) for col in zip(*grid)]
print(transposed == grid, [sum(r) for r in grid], [sum(c) for c in zip(*grid)])
rotated = [list(r) for r in zip(*grid[::-1])]
print(rotated[0], rotated[-1])
