# A calculator: a tokenizer, a recursive-descent parser making a tree of classes, and an
# evaluator with variables, functions and errors of its own.
import sys
class Token:
    def __init__(self, kind, text):
        self.kind, self.text = kind, text
    def __repr__(self):
        return f"{self.kind}:{self.text}"
def tokenize(src):
    tokens, i = [], 0
    while i < len(src):
        c = src[i]
        if c.isspace():
            i += 1
        elif c.isdigit() or c == ".":
            j = i
            while j < len(src) and (src[j].isdigit() or src[j] == "."):
                j += 1
            tokens.append(Token("num", src[i:j]))
            i = j
        elif c.isalpha():
            j = i
            while j < len(src) and src[j].isalnum():
                j += 1
            tokens.append(Token("name", src[i:j]))
            i = j
        elif c in "+-*/^()=,":
            tokens.append(Token("op", c))
            i += 1
        else:
            raise SyntaxError(f"unexpected {c!r} at {i}")
    return tokens
class Num:
    def __init__(self, v):
        self.v = v
    def eval(self, env):
        return self.v
class Var:
    def __init__(self, name):
        self.name = name
    def eval(self, env):
        if self.name not in env:
            raise NameError(f"unknown variable {self.name}")
        return env[self.name]
class BinOp:
    ops = {"+": lambda a, b: a + b, "-": lambda a, b: a - b, "*": lambda a, b: a * b, "/": lambda a, b: a / b, "^": lambda a, b: a ** b}
    def __init__(self, op, left, right):
        self.op, self.left, self.right = op, left, right
    def eval(self, env):
        return BinOp.ops[self.op](self.left.eval(env), self.right.eval(env))
class Call:
    funcs = {"max": max, "min": min, "abs": abs, "round": round}
    def __init__(self, name, args):
        self.name, self.args = name, args
    def eval(self, env):
        return Call.funcs[self.name](*[a.eval(env) for a in self.args])
class Parser:
    def __init__(self, tokens):
        self.tokens, self.pos = tokens, 0
    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None
    def take(self, text=None):
        t = self.peek()
        if t is None or (text is not None and t.text != text):
            raise SyntaxError(f"expected {text or 'more'}, got {t}")
        self.pos += 1
        return t
    def expr(self):
        node = self.term()
        while self.peek() and self.peek().text in "+-":
            node = BinOp(self.take().text, node, self.term())
        return node
    def term(self):
        node = self.power()
        while self.peek() and self.peek().text in "*/":
            node = BinOp(self.take().text, node, self.power())
        return node
    def power(self):
        node = self.atom()
        if self.peek() and self.peek().text == "^":
            self.take()
            return BinOp("^", node, self.power())
        return node
    def atom(self):
        t = self.take()
        if t.kind == "num":
            return Num(float(t.text) if "." in t.text else int(t.text))
        if t.text == "-":
            return BinOp("-", Num(0), self.atom())
        if t.text == "(":
            node = self.expr()
            self.take(")")
            return node
        if t.kind == "name":
            if self.peek() and self.peek().text == "(":
                self.take("(")
                args = [self.expr()]
                while self.peek() and self.peek().text == ",":
                    self.take()
                    args.append(self.expr())
                self.take(")")
                return Call(t.text, args)
            return Var(t.text)
        raise SyntaxError(f"unexpected {t}")
env = {"pi": 3.14159}
for line in ["1 + 2 * 3", "(1 + 2) * 3", "2 ^ 3 ^ 2", "x = 10", "x * pi", "max(1, x, 7) - min(4, 2)", "-x + 1", "7 / 2", "abs(-3.5)",
             "1 / 0", "y + 1", "3 +", "2 $ 2", "round(2.567, 2)"]:
    try:
        if "=" in line:
            name, src = line.split("=")
            env[name.strip()] = Parser(tokenize(src)).expr().eval(env)
            print(f"{name.strip()} := {env[name.strip()]}")
        else:
            print(f"{line} = {Parser(tokenize(line)).expr().eval(env)}")
    except (SyntaxError, NameError, ZeroDivisionError) as e:
        print(f"{line} -> {type(e).__name__}: {e}")
print(tokenize("a1 + 22*(b)"), file=sys.stdout)
