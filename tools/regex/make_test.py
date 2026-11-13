import re
import sys

# Makes tests/regex_java.ww: every case of tests/regex_cases.txt, with what Java answers (as
# Kotlin's Regex would) as its expect line. From the repository's top:
#
#   java -Dfile.encoding=UTF-8 -Dstdout.encoding=UTF-8 tools/regex/RegexCases.java tests/regex_cases.txt > java.txt
#   py tools/regex/make_test.py java.txt tests/regex_cases.txt tests/regex_java.ww
#
# A case file line is a pattern, its options (i m s x d l) and a text, between tabs; in the text
# \n, \t, \r, \\ and \u{hex} stand for what they say.
cases = open(sys.argv[2], encoding='utf-8').read().split('\n')
cases = [c for c in cases if c != '']
java = open(sys.argv[1], encoding='utf-8').read().split('\n')
java = [j for j in java if j != '']
assert len(cases) == len(java), (len(cases), len(java))


def decode(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n':
                out.append('\n'); i += 2; continue
            if n == 't':
                out.append('\t'); i += 2; continue
            if n == 'r':
                out.append('\r'); i += 2; continue
            if n == '\\':
                out.append('\\'); i += 2; continue
            if n == 'u':
                close = s.index('}', i)
                out.append(chr(int(s[i + 3:close], 16))); i = close + 1; continue
            out.append('\\' + n); i += 2; continue
        out.append(c)
        i += 1
    return ''.join(out)


def literal(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '$':
            out.append('\\$')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '\r':
            out.append('\\r')
        elif o < 0x20 or o in (0x85, 0x2028, 0x2029):
            out.append('\\u{%x}' % o)
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


lines = []
lines.append('// Regular expressions against Java: each case of regex_cases.txt (a pattern, its options, a')
lines.append('// text) run as Kotlin\'s Regex runs it on the JVM, Java 23\'s answers written beside it as the')
lines.append('// expected output. Made by a script from the cases and java.util.regex: the matches one after')
lines.append('// another as findAll finds them (each with its groups, positions by character), whether the')
lines.append('// whole text matches, the pieces split gives, and replace with "<$0>". A pattern Java refuses')
lines.append('// is refused here too, at the same place.')
lines.append('')
lines.append('fun show(s : string) : string {')
lines.append('  val out = StringBuilder()')
lines.append('  for (c in s) {')
lines.append('    if (c == \'\\n\') { out.append("\\\\n") } else if (c == \'\\r\') { out.append("\\\\r") } else if (c == \'\\t\') { out.append("\\\\t") }')
lines.append('    else if (c == \'\\\\\') { out.append("\\\\\\\\") }')
lines.append('    else if (c.code < 0x20 or c.code == 0x2028 or c.code == 0x85) { out.append("\\\\u{" + c.code.toString(16) + "}") }')
lines.append('    else { out.append(c) }')
lines.append('  }')
lines.append('  return out.toString()')
lines.append('}')
lines.append('')
lines.append('fun check(k : int, pattern : string, flags : string, input : string) {')
lines.append('  val options = []')
lines.append('  if (flags.contains("i")) { options.add(RegexOption.ignoreCase) }')
lines.append('  if (flags.contains("m")) { options.add(RegexOption.multiline) }')
lines.append('  if (flags.contains("s")) { options.add(RegexOption.dotMatchesAll) }')
lines.append('  if (flags.contains("x")) { options.add(RegexOption.comments) }')
lines.append('  if (flags.contains("d")) { options.add(RegexOption.unixLines) }')
lines.append('  if (flags.contains("l")) { options.add(RegexOption.literal) }')
lines.append('  var re : Regex? = null')
lines.append('  try {')
lines.append('    re = Regex(pattern, options.toSet())')
lines.append('  } catch (e : PatternSyntaxException) {')
lines.append('    print("#$k error ${e.index}")')
lines.append('    return')
lines.append('  }')
lines.append('  val r = re!!')
lines.append('  val out = StringBuilder("#$k find")')
lines.append('  var count = 0')
lines.append('  for (m in r.findAll(input)) {')
lines.append('    if (count == 50) { break }')
lines.append('    count += 1')
lines.append('    out.append(" [${m.range.start}-${m.range.end + 1}")')
lines.append('    for (g in 1 ..< m.groups.size) {')
lines.append('      val group = m.groups[g]')
lines.append('      out.append(group == null ? " _" : " ${group.range.start}-${group.range.end + 1}")')
lines.append('    }')
lines.append('    out.append("]")')
lines.append('  }')
lines.append('  out.append(" | matches=${r.matches(input)} | split=")')
lines.append('  for (part in r.split(input)) { out.append("<" + show(part) + ">") }')
lines.append('  out.append(" | replace=" + show(r.replace(input, "<\\$0>")))')
lines.append('  print(out.toString())')
lines.append('}')
lines.append('')
for k, (case, expect) in enumerate(zip(cases, java), 1):
    pattern, flags, inp = case.split('\t')
    assert expect.startswith('#%d ' % k), (k, expect)
    lines.append('check(%d, %s, %s, %s)   // expect: %s' % (k, literal(pattern), literal(flags), literal(decode(inp)), expect))
open(sys.argv[3], 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print(len(cases), 'cases')
