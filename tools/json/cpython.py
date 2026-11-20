# What CPython's json module gives for the cases tests/json.ww checks WinterWolf's Json against:
# reads.json the texts to read, reads.expected what reading each gives ("ok" and the value written
# back, or "err" and the message), writes.expected what writing the values of tests/json.ww gives
# (the same values, written here as Python's). Run in WSL, where CPython 3.14 is:
#
#   wsl -e python3.14 tools/json/cpython.py tests/json_cases
import json, os, sys

folder = sys.argv[1]

reads = [
 '', ' ', '[', '{', '{"a"', '{"a":', '{"a":1', '{"a":1,', '{"a":1,}', '[1,]', '[1 2]', '{"a" 1}', '{1:2}', '{"a":1 "b":2}',
 '"abc', '"a\\', '"a\\x"', '"a\\u12"', '"a\\u12g4"', '"a\tb"', '"a\nb"', '"\\ud83d\\ude00"', '"\\u00e9\\n\\/"',
 '01', '-', '-a', '1.', '.5', '1e', '1e+', '+1', '1.5e3', '-0', '-0.0', '1E400', '-1e400', '1e-400', '12345678901234567890123',
 '-9223372036854775809', '9223372036854775807', '-9223372036854775808',
 'NaN', 'Infinity', '-Infinity', 'nan', 'inf', 'tru', 'nul', 'null x', ' [1, 2] ', '﻿[1]', '[1]\n\n  x', '{"a":1,"a":2,"b":3}',
 '"\x7fé"', '[[[]]]', '{"":{}}', '"\\"\\\\\\b\\f\\r"', 'true', 'false', '[true,false,null]',
 '{"a" : 1 , "b" :[ 1 ,2 ] }', '\t\r\n[\n1\n]\n', '[1,\n2,\n3 x]', '{"a":\n\n  }', '"\\u0041\\u00DF\\u6771"', '[1e5, 1E-5, 0.5e+3, -0.0e0, 123.456]',
 '{"a":{"b":{"c":[1,{"d":"e"}]}}}', '[,]', '[1,,2]', '{,}', '{"a":1,,"b":2}', '"\\uD834\\uDD1E"', '[-]', '[0x10]', '[1.e5]',
 '""', '"\u0000"', '[" "]', '"\\u0000"', '["a"  ,  "b"]', '{"\\u0061":1}', '[100000000000000000000000000000000000000000.5]',
 '[1.7976931348623157e308, 2.2250738585072014e-308, 4.9406564584124654e-324]', '[0.1, 0.2, 0.30000000000000004, 1e22, 1e21, 123456789012345680000]',
]

with open(os.path.join(folder, "reads.json"), "w", encoding="utf-8", newline="\n") as f:
    f.write(json.dumps(reads, indent=1) + "\n")
with open(os.path.join(folder, "reads.expected"), "w", encoding="utf-8", newline="\n") as f:
    for r in reads:
        try:
            f.write("ok " + json.dumps(json.loads(r)) + "\n")
        except json.JSONDecodeError as e:
            f.write("err " + str(e) + "\n")

lines = []
def out(*parts):
    lines.append(" ".join(str(p) for p in parts))
writes = [
 None, True, 1, 0, 1.0, -0.0, 0.1, 1e-5, 1e16, 1e15, 123456789.0, 1.5e300, 5e-324, 2**64, -2**70, "a\"b\\c\n\t\b\f\r\x01\x7f", "é€\U0001F600",
 [], {}, [1, [2, []], {}], {"b": 1, "a": [1, 2], "c": {"d": None}}, {1: "x", 2.5: "y", False: "z", None: "w"}, [1e22, 1e21, 1e-4, 1e-5, 0.0001234, 1234567.125],
 {"z": {"y": [{"x": 1}, []]}, "a": {}},
]
for w in writes:
    plain = not isinstance(w, dict) or all(isinstance(k, str) for k in w)
    out("w", json.dumps(w), "|", json.dumps(w, ensure_ascii=False), "|", json.dumps(w, indent=2), "|",
        json.dumps(w, separators=(',', ':'), sort_keys=True) if plain else "-", "|", json.dumps(w, indent="\t", sort_keys=True) if plain else "-")
out("nan", json.dumps([float('nan'), float('inf'), -float('inf')]))
for bad in [float('nan'), float('inf'), -float('inf')]:
    try: json.dumps(bad, allow_nan=False)
    except ValueError as e: out("nanerr", e)
out("skip", json.dumps({(1, 2): 1, "a": 2}, skipkeys=True))
out("default", json.dumps({1, 2}, default=lambda o: sorted(o)))
a = []; a.append(a)
try: json.dumps(a)
except ValueError as e: out("circ", e)
out("indent0", json.dumps([1, {"a": 2}], indent=0))
out("indentneg", json.dumps([1, {"a": 2}], indent=-3))
out("sortnums", json.dumps({10: 1, 2: 2, 1: 3}, sort_keys=True))
out("seps", json.dumps({"a": [1, 2]}, separators=(" ; ", " = ")))
out("seps indent", json.dumps({"a": [1, 2]}, indent=1, separators=(" ; ", " = ")))
with open(os.path.join(folder, "writes.expected"), "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
