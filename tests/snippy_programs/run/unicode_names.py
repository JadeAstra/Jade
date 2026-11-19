# Names that are not ASCII, and f-strings as Python 3.12 reads them (the same quotes inside).
café = "coffee"
naïve_count = 3
π = 3.14159
def grüße(name):
    return f"Grüße, {name}!"
class Größe:
    def __init__(self, wert):
        self.wert = wert
    def doppelt(self):
        return Größe(self.wert * 2)
print(café, naïve_count, π, grüße("Welt"), Größe(21).doppelt().wert)
données = {"clé": "valeur"}
print(données["clé"], [λ * 2 for λ in range(3)])
items = {"a": 1, "b": 2}
print(f"{items["a"]} and {items['b']}", f"{'nested ' + f'{1 + 1}'}", f"{", ".join(["x", "y"])}")
print(f"{f"{f"{3}"}"}", f"{'\n'.join(['l1', 'l2'])}", f"{3.14159:{'.2f'}}", f"{'quote: "'}")
words = ["α", "β", "γ"]
print(f"{len(words)} letters: {', '.join(w.upper() for w in words)}", "".join(sorted("γβα")), "Ω".lower(), "ß".upper())
print("日本語"[1], len("日本語"), "日本" + "語", "語" in "日本語", "日本語".encode(), ord("語"))
