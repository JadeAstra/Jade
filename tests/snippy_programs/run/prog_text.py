# Text processing: word counts, ciphers, palindromes, anagrams, wrapping, a table.
text = """It was the best of times, it was the worst of times, it was the age of wisdom,
it was the age of foolishness, it was the epoch of belief, it was the epoch of incredulity,
it was the season of Light, it was the season of Darkness."""
words = [w.strip(".,").lower() for w in text.split()]
counts = {}
for w in words:
    counts[w] = counts.get(w, 0) + 1
top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:6]
print(len(words), len(counts), top)
for word, n in top:
    print(f"{word:<8}{n:>3} {'#' * n}")
def caesar(s, shift):
    out = []
    for ch in s:
        if ch.isalpha():
            base = ord("A") if ch.isupper() else ord("a")
            out.append(chr((ord(ch) - base + shift) % 26 + base))
        else:
            out.append(ch)
    return "".join(out)
secret = caesar("Hello, World!", 3)
print(secret, caesar(secret, -3), caesar("xyz", 3))
def is_palindrome(s):
    cleaned = [c.lower() for c in s if c.isalnum()]
    return cleaned == cleaned[::-1]
print([p for p in ["racecar", "A man, a plan, a canal: Panama", "hello", "Was it a car or a cat I saw?"] if is_palindrome(p)])
groups = {}
for w in ["listen", "silent", "enlist", "google", "gooegl", "cat", "act", "tac", "dog"]:
    groups.setdefault("".join(sorted(w)), []).append(w)
print([g for g in groups.values() if len(g) > 1])
def wrap(s, width):
    lines, line = [], ""
    for word in s.split():
        if line and len(line) + 1 + len(word) > width:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}" if line else word
    if line:
        lines.append(line)
    return lines
for line in wrap(text, 30):
    print(f"|{line:<30}|")
vowels = sum(1 for c in text.lower() if c in "aeiou")
print("vowels", vowels, "capitals", [c for c in text if c.isupper()], "lines", len(text.splitlines()))
print(text.title()[:40], text.count("it was"), text.lower().count("it was"), text.find("age"), text.rfind("age"))
csv = "name,age,city\nAnn,30,Paris\nBob,25,Rome\nCy,35,Oslo"
rows = [line.split(",") for line in csv.splitlines()]
header, *body = rows
records = [dict(zip(header, r)) for r in body]
print(records)
print(sorted(records, key=lambda r: int(r["age"]))[0]["name"], sum(int(r["age"]) for r in records) / len(records))
print(" | ".join(f"{h:^6}" for h in header))
for r in records:
    print(" | ".join(f"{r[h]:^6}" for h in header))
acronym = "".join(w[0] for w in "portable network graphics".split()).upper()
print(acronym, "snake_case_name".replace("_", " ").title().replace(" ", ""), "CamelCaseName"[0].lower() + "CamelCaseName"[1:])
