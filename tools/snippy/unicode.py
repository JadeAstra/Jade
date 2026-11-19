# Makes lib/Snippy/Unicode.ww: the general category of every code point, as the Unicode database
# of the CPython that runs this has it (CPython 3.14: Unicode 16.0), so that Snippy's answers are
# Python's (str.isprintable, repr, str.isalpha and the rest). From the repository's top, in WSL:
#
#   python3 tools/snippy/unicode.py > lib/Snippy/Unicode.ww
#
# The table is the runs of one category: each a letter for the category, then how far past the
# last run's start this one starts, in base 32 (digits 0-9 and a-v).
import unicodedata

CATEGORIES = ["Cc", "Cf", "Cn", "Co", "Cs", "Ll", "Lm", "Lo", "Lt", "Lu", "Mc", "Me", "Mn", "Nd", "Nl",
              "No", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps", "Sc", "Sk", "Sm", "So", "Zl", "Zp", "Zs"]
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!#$%"
DIGITS = "0123456789abcdefghijklmnopqrstuv"


def base32(n):
    if n == 0:
        return "0"
    out = ""
    while n:
        out = DIGITS[n % 32] + out
        n //= 32
    return out


runs = []
prev = None
for cp in range(0x110000):
    c = unicodedata.category(chr(cp))
    if c != prev:
        runs.append((cp, c))
        prev = c

parts = []
last = 0
for start, cat in runs:
    parts.append(LETTERS[CATEGORIES.index(cat)] + base32(start - last))
    last = start
table = "".join(parts)
lines = [table[i:i + 96] for i in range(0, len(table), 96)]

print("// Python's Unicode database, as Snippy needs it: the general category of every code point,")
print("// Unicode " + unicodedata.unidata_version + ", made by tools/snippy/unicode.py from the CPython it ran on. Do not edit.")
print("")
print("// The 30 general categories, in the order the table's letters stand for them.")
print("internal val __pyCategoryNames : List<string> = [" + ", ".join('"' + c + '"' for c in CATEGORIES) + "]")
print("")
print("// The runs, as tools/snippy/unicode.py writes them.")
print("internal val __pyCategoryTable : string =")
for i, line in enumerate(lines):
    print('  "' + line + '"' + (" +" if i < len(lines) - 1 else ""))
