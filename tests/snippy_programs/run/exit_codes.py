# How a program ends: sys.exit with a message, after finally blocks run and output is flushed.
import sys
def main():
    try:
        print("working")
        sys.exit("fatal: giving up")
    finally:
        print("finally still runs")
try:
    sys.exit(0)
except SystemExit as e:
    print("caught exit", e.code)
try:
    exit(5)
except SystemExit as e:
    print("caught builtin exit", e.code)
main()
print("never")
