#!/usr/bin/env python3
# The suite on Linux and macOS (G56), as tests/run.ps1 runs it on Windows: every tests/*.astra
# file run the ways asked for, with what its comments expect,
#
#   // expect: <line>                  the next line of standard output
#   // expect runtime error: <text>    the program fails (exit code 70) and reports <text>
#   // expect compile error: <text>    compilation fails (exit code 65) and reports <text>
#   // expect error output: <text>     the program finishes normally, having reported <text>
#
# then tests/check (`astra check`), tests/testing (`astra test`), tests/shell (the shell), the
# standard library and the examples checked whole, and the standard library described.
#
# A test that only means something on Windows says so in a line `// windows only: <why>`, and
# is passed over here, counted apart. A program built with `astra build --exe` is Windows's only
# so far, so the ways are script, cached, units and compiled.
#
#   python3 tests/run.py --astra build/linux/astra --parallel 16 --ways script

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ALL_WAYS = ["script", "cached", "units", "compiled"]


def run_astra(astra, args, directory, cache, stdin=None, timeout=600):
    env = dict(os.environ)
    env["ASTRA_CACHE"] = cache
    try:
        done = subprocess.run([astra] + args, cwd=directory, env=env, input=stdin, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"lines": [], "error": "timed out after %d s" % timeout, "code": -1}
    out = done.stdout.decode("utf-8", "replace")
    lines = re.split(r"\r?\n", out)
    if lines and lines[-1] == "":
        lines.pop()
    return {"lines": lines, "error": done.stderr.decode("utf-8", "replace"), "code": done.returncode}


def compare(run, expected, error_text, exit_code):
    actual = run["lines"]
    for i in range(max(len(expected), len(actual))):
        want = expected[i].rstrip() if i < len(expected) else "<no more output>"
        got = actual[i].rstrip() if i < len(actual) else "<no more output>"
        if want != got:
            return "output line %d:\n      expected: %s\n      actual:   %s" % (i + 1, want, got)
    if run["code"] != exit_code:
        return "exit code %d, expected %d\n      stderr: %s" % (run["code"], exit_code, run["error"].strip())
    if error_text and error_text not in run["error"]:
        return "stderr does not contain: %s\n      stderr: %s" % (error_text, run["error"].strip())
    if not error_text and run["error"].strip():
        return "unexpected stderr: %s" % run["error"].strip()
    return None


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def marker(text, pattern):
    found = re.search(pattern, text, re.M)
    return found.group(1).rstrip() if found else ""


def windows_only(text):
    return marker(text, r"// windows only: (.*)$")


def test_one(path, astra, directory, ways):
    os.makedirs(directory, exist_ok=True)
    cache = os.path.join(directory, "cache")
    name = os.path.splitext(os.path.basename(path))[0]
    text = read(path)
    expected = [m.group(1) for m in re.finditer(r"// expect: ?(.*)$", text, re.M)]
    runtime_error = marker(text, r"// expect runtime error: (.*)$")
    compile_error = marker(text, r"// expect compile error: (.*)$")
    bytecode = os.path.join(directory, name + ".astc")
    results = {}
    if compile_error:
        if "script" in ways:
            results["script"] = compare(run_astra(astra, [path], directory, cache), [], compile_error, 65)
        if "compiled" in ways:
            results["compiled"] = compare(run_astra(astra, ["build", path, "-o", bytecode], directory, cache), [], compile_error, 65)
        return results
    error_text = runtime_error or marker(text, r"// expect error output: (.*)$")
    exit_code = 70 if runtime_error else 0
    if {"script", "cached", "units"} & set(ways):
        first = compare(run_astra(astra, [path], directory, cache), expected, error_text, exit_code)
        if "script" in ways:
            results["script"] = first
    if "cached" in ways:
        results["cached"] = compare(run_astra(astra, [path], directory, cache), expected, error_text, exit_code)
    if "units" in ways:
        if os.path.isdir(cache):
            for kept in os.listdir(cache):
                if kept.endswith(".program"):
                    os.remove(os.path.join(cache, kept))
        results["units"] = compare(run_astra(astra, [path], directory, cache), expected, error_text, exit_code)
    if "compiled" in ways:
        build = run_astra(astra, ["build", path, "-o", bytecode], directory, cache)
        if build["code"] != 0:
            results["compiled"] = "build failed: " + build["error"].strip()
        else:
            results["compiled"] = compare(run_astra(astra, [bytecode], directory, cache), expected, error_text, exit_code)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--astra", required=True)
    parser.add_argument("--parallel", type=int, default=1)
    parser.add_argument("--ways", default=",".join(ALL_WAYS))
    parser.add_argument("--only", default="", help="the tests to run, by name, comma-separated")
    options = parser.parse_args()
    astra = os.path.abspath(options.astra)
    ways = [w.strip() for w in options.ways.split(",") if w.strip()]
    only = set(n.strip() for n in options.only.split(",") if n.strip())
    tmp = tempfile.mkdtemp(prefix="astra-tests-")
    passed, skipped, failures, alone_only = 0, [], [], []

    tests = sorted(p for p in (os.path.join(HERE, n) for n in os.listdir(HERE)) if p.endswith(".astra"))
    if only:
        tests = [t for t in tests if os.path.splitext(os.path.basename(t))[0] in only]
    runnable = []
    for test in tests:
        why = windows_only(read(test))
        if why:
            skipped.append("%s: %s" % (os.path.basename(test), why))
        else:
            runnable.append(test)

    def one(test, suffix=""):
        return test_one(test, astra, os.path.join(tmp, os.path.splitext(os.path.basename(test))[0] + suffix), ways)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, options.parallel)) as pool:
        outcomes = dict(zip(runnable, pool.map(one, runnable)))
    for test in runnable:
        results = outcomes[test]
        failed = [w for w, r in results.items() if r is not None]
        if options.parallel > 1 and failed:
            # Run again alone: a failure that goes away is the load of the others, not the test.
            results = one(test, "-alone")
            if not any(r is not None for r in results.values()):
                alone_only.append("%s [%s]" % (os.path.basename(test), ", ".join(failed)))
        for way, result in results.items():
            if result is None:
                passed += 1
            else:
                failures.append("%s [%s]\n    %s" % (os.path.basename(test), way, result))

    if not only:
        cache = os.path.join(tmp, "cache")
        # The type checker: a line ending in `// check: <message>` is reported with that message.
        check_dir = os.path.join(HERE, "check")
        for name in sorted(os.listdir(check_dir)):
            if not name.endswith(".astra"):
                continue
            path = os.path.join(check_dir, name)
            text = read(path)
            if windows_only(text):
                skipped.append("check/%s: %s" % (name, windows_only(text)))
                continue
            wanted = set()
            for number, line in enumerate(text.splitlines(), 1):
                found = re.search(r"// check: (.*)$", line)
                if found:
                    wanted.add("%d: %s" % (number, found.group(1).rstrip()))
            run = run_astra(astra, ["check", path], tmp, cache)
            problems, found_keys = [], set()
            for line in run["lines"]:
                m = re.match(r"^[^:]+:(\d+):\d+: error: (.*)$", line)
                if m:
                    key = "%s: %s" % (m.group(1), m.group(2).rstrip())
                    found_keys.add(key)
                    if key not in wanted:
                        problems.append("unexpected: " + key)
                elif line.strip():
                    problems.append("unexpected output: " + line)
            problems += ["not reported: " + k for k in sorted(wanted - found_keys)]
            code = 65 if wanted else 0
            if run["code"] != code:
                problems.append("exit code %d, expected %d; stderr: %s" % (run["code"], code, run["error"].strip()))
            if problems:
                failures.append("%s [check]\n    %s" % (name, "\n    ".join(problems)))
            else:
                passed += 1

        # `astra test`: its `// expect:` lines are what it prints after the line naming the file.
        testing_dir = os.path.join(HERE, "testing")
        for name in sorted(os.listdir(testing_dir)):
            if not name.endswith(".astra"):
                continue
            path = os.path.join(testing_dir, name)
            text = read(path)
            if windows_only(text):
                skipped.append("testing/%s: %s" % (name, windows_only(text)))
                continue
            expected = [m.group(1) for m in re.finditer(r"// expect: ?(.*)$", text, re.M)]
            run = run_astra(astra, ["test", path], tmp, cache)
            run["lines"] = run["lines"][1:]
            code = 1 if any(re.match(r"^\s*FAIL", e) for e in expected) else 0
            result = compare(run, expected, "", code)
            if result is None:
                passed += 1
            else:
                failures.append("%s [astra test]\n    %s" % (name, result))

        # The standard library, Jade and the examples pass the checker as they are.
        checked = [(os.path.join(HERE, "basics.astra"), "--std")]
        examples = os.path.join(HERE, "..", "examples")
        if os.path.isdir(examples):
            checked += [(os.path.join(examples, n), None) for n in sorted(os.listdir(examples)) if n.endswith(".astra")]
        for path, flag in checked:
            run = run_astra(astra, ["check"] + ([flag] if flag else []) + [path], tmp, cache)
            if run["code"] == 0:
                passed += 1
            else:
                failures.append("%s [checked whole]\n    exit code %d: %s" % (os.path.basename(path), run["code"], (" ".join(run["lines"]) + " " + run["error"]).strip()))

        # Every public declaration of the standard library is described.
        std = os.path.join(HERE, "..", "src", "std")
        if os.path.isdir(std):
            files = sorted(os.path.join(std, n) for n in os.listdir(std) if n.endswith(".astra"))
            run = run_astra(astra, ["doc", "--missing"] + files, tmp, cache)
            undescribed = [l for l in run["lines"] if l]
            if run["code"] == 0 and not undescribed:
                passed += 1
            else:
                failures.append("the standard library described [astra doc --missing]\n    " + "\n    ".join(undescribed + [run["error"]]))

        # The shell: each tests/shell/*.shell file typed into it.
        shell_dir = os.path.join(HERE, "shell")
        for name in sorted(os.listdir(shell_dir)):
            if not name.endswith(".shell"):
                continue
            text = read(os.path.join(shell_dir, name))
            want_out = [m.group(1) for m in re.finditer(r"(?m)^// expect: (.*?)\r?$", text)]
            want_err = [m.group(1) for m in re.finditer(r"(?m)^// expect error: (.*?)\r?$", text)]
            run = run_astra(astra, [], tmp, cache, stdin=text.encode("utf-8"))
            got_out = [l.rstrip() for l in run["lines"] if l != ""]
            got_err = [l.rstrip() for l in re.split(r"\r?\n", run["error"]) if l != ""]
            problem = None
            if got_out != want_out:
                problem = "output:\n      expected: %s\n      actual:   %s" % (want_out, got_out)
            elif got_err != want_err:
                problem = "errors:\n      expected: %s\n      actual:   %s" % (want_err, got_err)
            if problem:
                failures.append("%s [shell]\n    %s" % (name, problem))
            else:
                passed += 1

    for failure in failures:
        print("FAIL " + failure)
    for test in alone_only:
        print("passed only when run alone: " + test)
    for test in skipped:
        print("windows only: " + test)
    print("%d passed, %d failed, %d windows only" % (passed, len(failures), len(skipped)))
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
