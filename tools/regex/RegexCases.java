import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

// The regex cases (tests/regex_cases.txt) as Kotlin's Regex answers them on the JVM: find and
// next as Kotlin's findAll, split as Kotlin's, replace as Matcher.replaceAll. One line a case.
public class RegexCases {
    static String decode(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char n = s.charAt(++i);
                switch (n) {
                    case 'n': b.append('\n'); break;
                    case 't': b.append('\t'); break;
                    case 'r': b.append('\r'); break;
                    case '\\': b.append('\\'); break;
                    case 'u': {
                        int close = s.indexOf('}', i);
                        b.appendCodePoint(Integer.parseInt(s.substring(i + 2, close), 16));
                        i = close;
                        break;
                    }
                    default: b.append('\\').append(n);
                }
            } else {
                b.append(c);
            }
        }
        return b.toString();
    }

    static String show(String s) {
        StringBuilder b = new StringBuilder();
        s.codePoints().forEach(c -> {
            if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c == '\t') b.append("\\t");
            else if (c == '\\') b.append("\\\\");
            else if (c < 0x20 || c == 0x2028 || c == 0x85) b.append(String.format("\\u{%x}", c));
            else b.appendCodePoint(c);
        });
        return b.toString();
    }

    // Positions in code points, as Astra counts them.
    static int cp(String s, int unit) { return s.codePointCount(0, unit); }

    public static void main(String[] args) throws Exception {
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        int k = 0;
        for (String line : lines) {
            k++;
            String[] f = line.split("\t", -1);
            String pattern = f[0], flags = f[1], input = decode(f[2]);
            int bits = 0;
            if (flags.contains("i")) bits |= Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE;
            if (flags.contains("m")) bits |= Pattern.MULTILINE;
            if (flags.contains("s")) bits |= Pattern.DOTALL;
            if (flags.contains("x")) bits |= Pattern.COMMENTS;
            if (flags.contains("d")) bits |= Pattern.UNIX_LINES;
            if (flags.contains("l")) bits |= Pattern.LITERAL;
            StringBuilder out = new StringBuilder("#" + k);
            Pattern p;
            try {
                p = Pattern.compile(pattern, bits);
            } catch (PatternSyntaxException e) {
                System.out.println(out + " error " + e.getIndex());
                continue;
            }
            try {
                // findAll, as Kotlin: find, then next() from the end (one on after an empty match).
                Matcher m = p.matcher(input);
                StringBuilder found = new StringBuilder();
                int from = 0;
                int count = 0;
                while (from <= input.length() && count < 50) {
                    m = p.matcher(input);
                    if (!m.find(from)) break;
                    count++;
                    found.append(" [").append(cp(input, m.start())).append("-").append(cp(input, m.end()));
                    for (int g = 1; g <= m.groupCount(); g++) {
                        found.append(m.start(g) < 0 ? " _" : " " + cp(input, m.start(g)) + "-" + cp(input, m.end(g)));
                    }
                    found.append("]");
                    from = m.end() + (m.end() == m.start() ? 1 : 0);
                }
                out.append(" find").append(found);
                out.append(" | matches=").append(p.matcher(input).matches());
                // Kotlin's split.
                Matcher s = p.matcher(input);
                List<String> parts = new ArrayList<>();
                int last = 0;
                while (s.find()) {
                    parts.add(input.substring(last, s.start()));
                    last = s.end();
                }
                parts.add(input.substring(last));
                out.append(" | split=");
                for (String part : parts) out.append("<").append(show(part)).append(">");
                out.append(" | replace=").append(show(p.matcher(input).replaceAll("<$0>")));
            } catch (StackOverflowError e) {
                out.append(" overflow");
            }
            System.out.println(out);
        }
    }
}
