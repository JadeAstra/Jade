# A game's logic: tic-tac-toe played out by a simple strategy, and a text adventure's rooms and
# commands, with match, dicts, classes and a deterministic random generator of its own.
class Rng:
    def __init__(self, seed):
        self.state = seed
    def next(self):
        self.state = (self.state * 1103515245 + 12345) % 2 ** 31
        return self.state
    def choice(self, items):
        return items[self.next() % len(items)]
LINES = [(0, 1, 2), (3, 4, 5), (6, 7, 8), (0, 3, 6), (1, 4, 7), (2, 5, 8), (0, 4, 8), (2, 4, 6)]
def winner(board):
    for a, b, c in LINES:
        if board[a] != " " and board[a] == board[b] == board[c]:
            return board[a]
    return None
def best_move(board, me, rng):
    other = "O" if me == "X" else "X"
    free = [i for i, v in enumerate(board) if v == " "]
    for who in (me, other):
        for i in free:
            trial = board[:]
            trial[i] = who
            if winner(trial) == who:
                return i
    if 4 in free:
        return 4
    return rng.choice(free)
def play(seed):
    board, player, rng = [" "] * 9, "X", Rng(seed)
    while winner(board) is None and " " in board:
        board[best_move(board, player, rng)] = player
        player = "O" if player == "X" else "X"
    return board, winner(board)
for seed in (1, 7, 42):
    board, won = play(seed)
    print(f"seed {seed}: {won or 'draw'}")
    for r in range(3):
        print(" " + " | ".join(board[3 * r:3 * r + 3]))
class Room:
    def __init__(self, name, description, exits, items=()):
        self.name, self.description, self.exits, self.items = name, description, exits, list(items)
rooms = {
    "hall": Room("hall", "A long hall.", {"north": "library", "east": "kitchen"}, ["lamp"]),
    "library": Room("library", "Dusty shelves.", {"south": "hall"}, ["book", "key"]),
    "kitchen": Room("kitchen", "Smells of bread.", {"west": "hall"}),
}
class Player:
    def __init__(self):
        self.room = "hall"
        self.inventory = []
    def act(self, command):
        here = rooms[self.room]
        match command.split():
            case ["look"]:
                return f"{here.description} Exits: {', '.join(sorted(here.exits))}. Items: {here.items or 'none'}"
            case ["go", direction] if direction in here.exits:
                self.room = here.exits[direction]
                return f"You walk {direction} to the {self.room}."
            case ["go", direction]:
                return f"You cannot go {direction}."
            case ["take", item] if item in here.items:
                here.items.remove(item)
                self.inventory.append(item)
                return f"Taken: {item}."
            case ["take", item]:
                return f"There is no {item} here."
            case ["inventory" | "inv"]:
                return f"You carry {sorted(self.inventory)}."
            case _:
                return "What?"
p = Player()
for cmd in ["look", "take lamp", "go north", "look", "take key", "take sword", "go west", "go south", "go east", "inv", "dance"]:
    print(f"> {cmd}\n{p.act(cmd)}")
