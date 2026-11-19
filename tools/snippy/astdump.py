# What Snippy's reader is measured by (the Snippy plan, part 5.1): every .py file of the standard
# library of the CPython that runs this, copied into a folder with its ast.dump beside it
# (name.py.dump), for tools/snippy/compare.ww to read the copies and compare. In WSL:
#
#   python3 tools/snippy/astdump.py <out folder> [<folder of .py files>]
#   python3 tools/snippy/astdump.py --beside tests/python
#
# With --beside, the dumps are written beside the files of the folder given, nothing copied (the
# suite's tests/python, for tests/snippy_read.ww).
#
# The second folder is the standard library's (sysconfig's stdlib path) when none is given.
import ast
import os
import shutil
import sys
import sysconfig

beside = sys.argv[1] == "--beside"
out = sys.argv[2] if beside else sys.argv[1]
root = out if beside else sys.argv[2] if len(sys.argv) > 2 else sysconfig.get_paths()["stdlib"]
count = 0
for folder, _, names in os.walk(root):
    for name in sorted(names):
        if not name.endswith(".py"):
            continue
        path = os.path.join(folder, name)
        rel = os.path.relpath(path, root)
        target = os.path.join(out, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(path, encoding="utf-8") as f:
            source = f.read()
        if not beside:
            shutil.copyfile(path, target)
        try:
            dump = ast.dump(ast.parse(source, rel))
        except SyntaxError as e:
            dump = "SyntaxError: " + str(e)
        with open(target + ".dump", "w", encoding="utf-8") as f:
            f.write(dump)
        count += 1
print(count, "files from", root)
