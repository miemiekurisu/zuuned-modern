# -*- coding: utf-8 -*-
"""Audit that interface wording is data, not code.

    python i18n/audit.py            report; exit 1 when a check fails
    python i18n/audit.py --list     print the source line beside every finding

The rule this enforces: a sentence the user reads belongs in i18n/zh_CN.tsv.
Wording written straight into a QML property or a C++ string literal can never
be translated, so it is a defect whether or not anyone happens to want a second
language.  See docs/I18N.md for the catalogue and src/UiText.h for the door C++
wording leaves through.

Four checks:

  catalogue  every source string lupdate found carries Chinese in the .ts.  An
             unfinished entry renders as English, which is the bug.
  tsv        the language file is well formed and every line names a source that
             still exists.  A stale line is a warning: dead weight, not a fault
             the user can see.
  qml        a quoted literal on a user-facing QML property (text:, title:,
             placeholderText:, toolTip:, ...) must sit inside qsTr().  QML and
             the handful of qml/*.js helpers are both scanned, because a label
             built in a .js table reaches a text: property just as easily.
  cxx        a sentence-shaped quoted literal under src/ or transcode/ must sit
             inside UiText::tr() / tr() / qsTr(), or be named in
             i18n/audit-allow.txt together with a reason.  A call written across
             lines counts as one literal: the continuation lines hold no string
             that lupdate would extract on its own.

Both text checks are shape heuristics and say so.  "Sentence-shaped" is three or
more alphabetic words; QML flags a literal that has a space in it, or one that
starts with a capital, unless it has the shape of a value (a colour, a URL, a
resource path, a glyph escape, a bare number or punctuation, a lowerCamel
enum).  So a two-word lowercase button label slips past, and a metadata constant
gets flagged and lands in the allow file.  That is the bargain: the allow file is
the review surface, and it is short enough to actually read.
"""
import argparse
import os
import re
import sys
from xml.sax.saxutils import unescape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TSV = os.path.join(HERE, "zh_CN.tsv")
TS = os.path.join(HERE, "zuuned_zh_CN.ts")
ALLOW = os.path.join(HERE, "audit-allow.txt")
SEP = " ~=> "
BS = chr(92)
TR_WRAP = re.compile(r"(?:qsTr|UiText::tr|QCoreApplication::translate|QT_TRANSLATE\w*|[A-Za-z_]\w*::tr|\btr)\s*\(\s*(?:C_[FTUI]?8)?\s*(\"(?:[^\"\\]|\\.)*\")")
QUOTED = re.compile(r"\"(?:[^\"\\]|\\.)*\"")
VALUE_SHAPE = re.compile(
    r"^(?:"
    r"#[0-9a-fA-F]{3,8}"                    # colour
    r"|[0-9.,%+\-/*x\s]+"                    # number or separator
    r"|[%][0-9].*"                           # format placeholder only
    r"|[a-z][A-Za-z0-9]*"                    # lowerCamel enum / css keyword
    r"|[a-z][a-z0-9_.\-]*[:/][^\s]*"         # url, scheme, resource path
    r"|[:/\\\\][^\s]*"                       # absolute or qrc path
    r"|\\\\u[0-9a-fA-F]{4}.*"                # glyph escape
    r"|[^A-Za-z]+"                           # no letters at all
    r")$"
)
# A line that is nothing but string-literal fragments: the tail of a multi-line
# fprintf/SQL statement whose opening line is already excluded.
CONTINUATION = re.compile(r"""^[ 	]*(?:L?"(?:[^"\\]|\\.)*"[ 	]*)+[+,=]?[ 	]*[,:]?[ 	]*$""")
# The same shape with the call-closing punctuation on the last fragment, which
# is how a sentence written across lines usually ends.  The one hole this
# leaves is a literals-only line sitting inside a translated call that also
# takes a second, untranslated argument; lupdate cannot extract that argument
# either, so nothing reaches the user that the catalogue did not already miss.
TR_TAIL = re.compile(r"""^[ 	]*(?:L?"(?:[^"\\]|\\.)*"[ 	]*)+[+,=]?[ 	]*[,:]?[ 	]*\)?;?$""")

UI_PROP = re.compile(
    r"^[ \t]*(?:(?:required[ \t]+|readonly[ \t]+property[ \t]+|property[ \t]+|alias[ \t]+)?"
    r"(text|title|placeholderText|toolTip|accessibleName|accessibleText|placeholder"
    r"|heading|description|summaryText|detailText|subText|caption))[ \t]*:"
)


def norm(t):
    return re.sub(r"\s+", " ", t).strip()


def read(path):
    with open(path, encoding="utf-8-sig") as f:
        return f.read()


def stripped(line):
    """The line with every already-translated literal blanked out."""
    return TR_WRAP.sub(chr(34) + chr(34), line)


def decomment(text):
    """Blank out // and block comments, keeping the line count.

    A sentence in a comment is a comment, not wording the user reads; without
    this the check spends its whole signal budget on prose written to the next
    programmer.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == chr(34):                       # string literal - copy verbatim
            out.append(c)
            i += 1
            while i < n:
                if text[i] == chr(92) and i + 1 < n:
                    out.append(text[i:i + 2]); i += 2; continue
                out.append(text[i])
                if text[i] in chr(34) + chr(10):
                    i += 1; break
                i += 1
            continue
        if c == chr(39):                       # char literal
            out.append(c); i += 1
            while i < n:
                if text[i] == chr(92) and i + 1 < n:
                    out.append(text[i:i + 2]); i += 2; continue
                out.append(text[i])
                if text[i] == chr(39):
                    i += 1; break
                i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != chr(10):
                out.append(" "); i += 1
            continue
        if text.startswith("/*", i):
            while i < n and not text.startswith("*/", i):
                out.append(text[i] if text[i] == chr(10) else " "); i += 1
            out.append("  "); i += 2
            continue
        out.append(c); i += 1
    return "".join(out)


def load_allow():
    """The review surface: exact/regex literals, whole files, and line shapes.

    SKIP-FILE :: <repo-relative path> :: <reason>
    SKIP-LINE :: <regex> :: <reason>      (a line matching it is not interface
                                          wording at all - a log, SQL, a probe)
    <literal-or-/regex/> :: <reason>
    """
    allow, skip, lines = {}, set(), []
    if os.path.exists(ALLOW):
        for line in read(ALLOW).split("\n"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            pat, sep, why = line.partition(" :: ")
            if not sep:
                continue
            kind = pat.strip()
            if kind in ("SKIP-FILE", "SKIP-LINE"):
                rest = why.split(" :: ")
                target, reason = rest[0].strip(), " :: ".join(rest[1:]).strip()
                if not target:
                    continue
                if kind == "SKIP-FILE":
                    skip.add(target)
                else:
                    lines.append((re.compile(target), reason or "(no reason given)"))
                continue
            allow[norm(pat)] = why.strip() or "(no reason given)"
    return allow, skip, lines


def logged(line, skip_lines):
    return any(rx.search(line) for rx, _ in skip_lines)


def allowed(lit, allow):
    key = norm(lit)
    if key in allow:
        return True
    for pat in allow:
        if pat.startswith("/") and pat.endswith("/") and re.search(pat[1:-1], key):
            return True
    return False


def ts_entries():
    """Every (context, source, translation) triple in the catalogue."""
    s = read(TS)
    pat = re.compile(r"<source>(.*?)</source>\s*<translation(?:\s+type=\"unfinished\")?"
                     r"\s*(?:/>|></translation>|>(.*?)</translation>)", re.S)
    out = []
    for ctx_m in re.finditer(r"<context>\s*<name>(.*?)</name>", s, re.S):
        ctx = unescape(ctx_m.group(1), {"&apos;": "'"})
        end = s.find("</context>", ctx_m.end())
        body = s[ctx_m.end():end if end >= 0 else len(s)]
        for m in pat.finditer(body):
            src = norm(unescape(m.group(1), {"&apos;": "'"}).replace("\n", " "))
            out.append((ctx, src, (m.group(2) or "").strip()))
    return out


def ts_sources():
    return [(src, tr) for _, src, tr in ts_entries()]


def check_catalogue(sources):
    bad = 0
    for src, tr in sources:
        if not tr:
            bad += 1
            print("  UNFINISHED: " + src[:110])
    print("catalogue: %d source(s), %d without Chinese" % (len(sources), bad))
    return bad == 0


def check_tsv(entries):
    """The language file, line by line, against what the catalogue really holds.

    A line is `source ~=> Chinese` or `Context!source ~=> Chinese`.  The second
    form is for one English word that means two things -- `Albums` is 专辑 next to
    artists and 相册 next to photos -- and a prefix only counts when it names a
    <context> that exists, so wording containing `!` still resolves as itself.
    """
    ok = True
    live_pairs = set()
    live_srcs = set()
    contexts = set()
    for ctx, src, _ in entries:
        contexts.add(ctx)
        live_pairs.add((ctx, src.replace(BS + "n", " ")))
        live_srcs.add(src.replace(BS + "n", " "))
    seen = {}
    for n, line in enumerate(read(TSV).split("\n"), 1):
        if not line.strip():
            continue
        if SEP not in line:
            print("  TSV:%d no ' ~=> ' separator: %s" % (n, line[:80]))
            ok = False
            continue
        src, zh = line.split(SEP, 1)
        key = norm(src)
        if not zh.strip():
            print("  TSV:%d empty translation for %s" % (n, key[:80]))
            ok = False
        if key in seen:
            print("  TSV:%d duplicate source, also line %d: %s" % (n, seen[key], key[:80]))
            ok = False
        seen[key] = n
    bare = set()
    scoped = set()
    for k in seen:
        head, sep, tail = k.partition("!")
        flat = k.replace(BS + "n", " ")
        if sep and head in contexts:
            scoped.add((head, tail.replace(BS + "n", " ")))
        else:
            bare.add(flat)
    missing = sorted(ctx + "!" + src for ctx, src in live_pairs
                     if src not in bare and (ctx, src) not in scoped)
    live_scoped = set(ctx + "!" + src for ctx, src in live_pairs)
    stale = sorted(k for k in seen
                   if k.replace(BS + "n", " ") not in live_scoped
                   and k.replace(BS + "n", " ") not in live_srcs)
    for k in missing:
        print("  MISSING: %s" % k[:110])
    for k in stale:
        print("  STALE (warning) tsv:%d names nothing any more: %s" % (seen[k], k[:90]))
    print("tsv: %d line(s), %d scoped, %d missing, %d stale"
          % (len(seen), len(scoped), len(missing), len(stale)))
    return ok and not missing


# A line that can only be the tail of the binding above it: it opens with an
# operator, a quote or a closing paren. QML has no statement terminator, so a
# ternary split as `? a` / `: b` across two lines is one binding, and reading
# only the first line is how an English sentence hid in plain sight (2026-10-24).
# Anything else - `name:`, `Name {`, `}`, a comment - starts something new.
QML_TAIL = re.compile(r"""^[ \t]*(?:[+?:.]+|[&|]{1,2}|[)=<>!]|["\x27])""")

def bracket_delta(line):
    """How many brackets a QML line opens but does not close.

    A property binding spread over four lines is one expression, and reading it
    one line at a time is how a ternary's second arm became an untranslated
    English sentence that the audit swore it had looked at (2026-10-24). Strings
    go first so a bracket inside wording cannot open anything.
    """
    masked = re.sub(r"//.*$", "", QUOTED.sub(chr(34) * 2, line))
    return (sum(masked.count(c) for c in "([{")
            - sum(masked.count(c) for c in ")]}"))


def mask_comparisons(text):
    """Blank a literal that is an operand of ==, !=, === or !==.

    A literal you compare against is a value token - a pivot name, a tab id -
    and translating it would break the comparison while doing nothing for the
    reader. `root.pivot === "Albums"` therefore stays out of the catalogue;
    the qsTr() on the other side of the same ternary is still checked.
    """
    blank = chr(34) * 2
    # QUOTED is reused rather than rewritten so this file never has to nest a
    # double quote inside a raw string to say "a string literal".
    after = re.compile(r"(===|!==|==|!=)\s*" + QUOTED.pattern)
    before = re.compile(QUOTED.pattern + r"\s*(===|!==|==|!=)")
    text = after.sub(lambda m: m.group(1) + " " + blank, text)
    text = before.sub(lambda m: blank + " " + m.group(1), text)
    return text


def check_qml(allow, skip, skip_lines, findings):
    hits = 0
    qml = os.path.join(ROOT, "qml")
    for name in sorted(f for f in os.listdir(qml) if f.endswith((".qml", ".js"))):
        rel = "qml/" + name
        if rel in skip:
            continue
        lines = read(os.path.join(qml, name)).split("\n")
        n = 0
        while n < len(lines):
            n += 1
            line = lines[n - 1]
            if not UI_PROP.match(line):
                continue
            if logged(line, skip_lines):
                continue
            # Carry the binding across lines until its brackets close again.
            start = n
            parts = [stripped(line).split(":", 1)[1]]
            balance = bracket_delta(line)
            j = n
            while j < len(lines):
                nxt = lines[j]
                if balance <= 0 and not QML_TAIL.match(nxt):
                    break
                if UI_PROP.match(nxt):
                    break
                parts.append(stripped(nxt))
                j += 1
                balance += bracket_delta(nxt)
            n = j
            rest = mask_comparisons(" ".join(parts))
            for lit in QUOTED.findall(rest):
                inner = lit[1:-1]
                if not re.search(r"[A-Za-z]{2}", inner):
                    continue
                if VALUE_SHAPE.match(inner):
                    continue
                if not (" " in inner or inner[0].isupper()):
                    continue
                if allowed(inner, allow):
                    continue
                hits += 1
                findings.append((rel, start, inner, line.strip()))
    print("qml: %d unwrapped literal(s) on a user-facing property" % hits)
    return hits == 0


def source_files(dirs):
    out = []
    for base in dirs:
        for dp, dn, fn in os.walk(os.path.join(ROOT, base)):
            dn[:] = [d for d in dn if d not in ("build", "art5", "art6")]
            for f in fn:
                if f.endswith((".cpp", ".h", ".c", ".hpp")):
                    rel = os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, "/")
                    out.append(rel)
    return sorted(out)


def check_cxx(allow, skip, skip_lines, findings):
    hits = 0
    for rel in source_files(("src", "transcode")):
        if rel in skip:
            continue
        body = decomment(read(os.path.join(ROOT, rel)))
        prev_skip = False
        prev_tr = False
        for n, line in enumerate(body.split("\n"), 1):
            drop = logged(line, skip_lines)
            if not drop and prev_skip and CONTINUATION.match(line):
                drop = True
            s = stripped(line)
            # C concatenates adjacent literals, so the tail of a sentence written
            # across lines is the same catalogue entry as its head:
            #     UiText::tr("first half "
            #                "second half")
            # lupdate extracts one source text, and the runtime looks up that one
            # text. Only a line that is nothing but literals and separators (a
            # continuation cannot stand as a statement) following a line that
            # really opens a translated call counts, so a fresh literal on its own
            # line is still a finding.
            continued = bool(TR_TAIL.match(s)) and prev_tr
            prev_tr = bool(TR_WRAP.search(line)) or continued
            prev_skip = drop
            if drop or continued:
                continue
            for lit in QUOTED.findall(s):
                inner = lit[1:-1]
                if " " not in inner:
                    continue        # one token: a pattern, a key, a path, an id
                words = re.findall(r"[A-Za-z][A-Za-z'’.-]{1,}", inner)
                if len(words) < 3:
                    continue
                if allowed(inner, allow):
                    continue
                hits += 1
                findings.append((rel, n, inner, line.strip()))
    print("cxx: %d sentence-shaped literal(s) outside a catalogue lookup" % hits)
    return hits == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="print the offending line too")
    args = ap.parse_args()
    if not os.path.exists(TS):
        print("no %s - run i18n/update.ps1 first" % TS)
        return 1
    allow, skip, skip_lines = load_allow()
    sources = ts_sources()
    findings = []
    results = [
        ("catalogue", check_catalogue(sources)),
        ("tsv", check_tsv(ts_entries())),
        ("qml", check_qml(allow, skip, skip_lines, findings)),
        ("cxx", check_cxx(allow, skip, skip_lines, findings)),
    ]
    if args.list:
        for p, n, inner, line in findings:
            print("\n%s:%d  \"%s\"\n    %s" % (p, n, inner[:100], line[:200]))
    ok = all(r for _, r in results)
    print("audit-allow: %d pattern(s), %d file(s), %d line rule(s)"
          % (len(allow), len(skip), len(skip_lines)))
    print("AUDIT=" + ("ok" if ok else "fail"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
