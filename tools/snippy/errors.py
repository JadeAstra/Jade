# What CPython says of each case of a file of broken programs (cases between lines of ===): its
# error's kind, message, line and column (from 1, in characters), one line a case, for
# tests/snippy_errors.ww to compare Snippy's with, as ast.parse raises them. In WSL:
#
#   python3 tools/snippy/errors.py tests/python/errors.txt
import ast
import sys

text = open(sys.argv[1], encoding="utf-8").read()
cases = text.split("\n===\n")
for case in cases:
    try:
        ast.parse(case, "<case>")   # the parser's errors: the compiler's own (return outside a function) are part 5.2's
        print("no error")
    except SyntaxError as e:
        line = (e.text or "").split("\n")[0] if e.text else ""
        print("%s: %s (%d:%d)" % (type(e).__name__, e.msg, e.lineno or 0, e.offset or 0))
