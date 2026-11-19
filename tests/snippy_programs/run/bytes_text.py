# String escapes and raw strings, and bytes: literals, indexing, slicing, encoding and back.
print("tab\there", "new\nline", "back\\slash", "quote\"s", 'single\'s', "bell\a".encode(), "\x41é\U0001F600", "\101")
print(len("\n"), len(r"\n"), r"raw\t", R"RAW\n", "a\
b")
print(repr("\x00\x1f\x7f\x80\xff"), repr(" ­​"), repr("é ñ 中 😀"), repr("\ud800" if False else "ok"))
b = b"hello"
print(b, b[0], b[1:3], b[-1], len(b), list(b), b + b" world", b * 2, b"l" in b, 104 in b, b.upper(), b.decode())
print(bytes([72, 105]), bytes(3), bytes("é", "utf-8"), "é".encode(), "é".encode("latin-1"), b"\xc3\xa9".decode("utf-8"))
print(b"abc" == b"abc", b"abc" < b"abd", b"a" != "a", repr(b"\x00\n\t'\""), b"x".hex(), bytes.fromhex if False else "fromhex")
print(b"a,b,c".split(b","), b"  x  ".strip(), b"abc".startswith(b"ab"), b"hello".find(b"l"), b"hello".count(b"l"), b"-".join([b"a", b"b"]))
ba = bytearray(b"abc")
ba[0] = 65
ba.extend(b"de") if False else None
print(ba, bytes(ba), len(ba), ba[1])
print("a😀b".encode("utf-8"), len("a😀b".encode()), "ascii".encode("ascii"), "Ünïcödé".encode("ascii", "replace"), "Ünïcödé".encode("ascii", "ignore"))
try:
    "é".encode("ascii")
except UnicodeEncodeError as e:
    print("UnicodeEncodeError:", e)
try:
    b"\xff".decode("ascii")
except UnicodeDecodeError as e:
    print("UnicodeDecodeError:", e)
print(str(b"bytes"), "%s" % b"x", f"{b'y'}", int.from_bytes(b"\x01\x00", "big"), (256).to_bytes(2, "little"), (-1).to_bytes(1, "big", signed=True))
s = "Ünïcödé text"
print(s.upper(), s.lower(), s.title(), s.casefold(), len(s), s[0], s.isalpha(), s.encode()[:4])
print("named escapes in 5.3","\t".isspace(), " ".isspace(), "　".isspace(), "x​y".split())
