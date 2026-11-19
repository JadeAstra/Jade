# The places of every node, for tools/snippy/compare.ww --positions: beside each .py file in a
# folder made by astdump.py, name.py.pos, a line for each node that has a place, in ast.walk's
# order: its class, then line:column-endline:endcolumn, the columns counted in characters (as
# Snippy counts them) rather than CPython's UTF-8 bytes. In WSL:
#
#   python3 tools/snippy/astpos.py <folder>
import ast
import os
import sys

root = sys.argv[1]
count = 0
for folder, _, names in os.walk(root):
    for name in names:
        if not name.endswith(".py"):
            continue
        path = os.path.join(folder, name)
        with open(path, encoding="utf-8") as f:
            source = f.read()
        lines = [line + "\n" for line in source.split("\n")]   # only \n ends a line in Python's places

        def chars(line, col):
            if line - 1 >= len(lines):
                return col
            return len(lines[line - 1].encode("utf-8")[:col].decode("utf-8", errors="replace"))

        out = []
        try:
            tree = ast.parse(source)
        except SyntaxError:
            continue
        for node in ast.walk(tree):
            if "lineno" in node._attributes and hasattr(node, "lineno"):
                out.append("%s %d:%d-%d:%d" % (type(node).__name__, node.lineno, chars(node.lineno, node.col_offset),
                                               node.end_lineno, chars(node.end_lineno, node.end_col_offset)))
        with open(path + ".pos", "w", encoding="utf-8") as f:
            f.write("\n".join(out))
        count += 1
print(count, "files")
