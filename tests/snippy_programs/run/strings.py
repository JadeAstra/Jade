# Strings: indexing by code point, slices, operators, comparisons, unicode.
s = "Hello, Wörld! 😀"
print(s, len(s), s[0], s[-1], s[7:12], s[::-1], s[::2], s[100:], s[-3:])
print(s[1:-1:3], s[:0], s[5:2], s[-100:3])
print("ab" + "cd", "ab" * 3, 3 * "x", "-" * 0, "a" < "b", "B" < "a", "abc" < "abd", "ab" < "abc", "é" > "z")
print("ell" in s, "xyz" not in s, "" in s, s.count("l"), s.find("W"), s.index("o"))
print(repr("it's"), repr('say "hi"'), repr("both ' and \""), repr("tab\tnew\nline\\"), repr("\x00\x7f\u200b\xa0é😀"))
print(str(123), str(1.5), str([1, "a"]), str(None), repr([1, "a", 2.5, None]))
for ch in "añ😀":
    print(ch, ord(ch), chr(ord(ch)), hex(ord(ch)))
print(chr(65), chr(0x1F600), ord("€"))
t = "abc"
t += "def"
print(t, t.upper(), t[::-1], sorted("banana"), list("abc"), "".join(reversed("abc")))
print(max("hello"), min("hello"), "-".join(["a", "b", "c"]), ",".join("xyz"))
print("a\tb".expandtabs(4), "x".center(7, "*"), "5".zfill(3), "-5".zfill(4), "abc".ljust(5) + "|", "abc".rjust(5))
multi = """line one
line two
  indented"""
print(multi)
print(multi.splitlines(), len(multi.splitlines()))
raw = r"C:\new\table"
print(raw, len(raw), "\\n" == r"\n")
print("%s and %r" % ("x", "y"), "a" "b" "c", ("implicit "
       "concatenation"))
