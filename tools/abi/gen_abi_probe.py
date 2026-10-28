#!/usr/bin/env python3
"""Reads the PUBLIC C headers of the engine (include/nue/*.h) and writes a C++ probe for the ABI guard test (tests/test_abi.cpp).

The probe lists, for every struct/union typedef: sizeof, alignof and the offset + size of every member (nested members as dotted paths);
for every enum: the value of each enumerator; for every numeric #define NUE_*: its value; and the names of all exported functions.
The test compares that table with the committed reference (tests/abi/reference/<layout>.txt). See docs/ABI.md, "ABI guard".

Only the Python standard library is used. Usage:  gen_abi_probe.py <include dir> <output .inc>
"""
import os
import re
import sys

# Headers that make up the stable C ABI. Aggregators (nue.h) and entry points contribute nothing but are scanned anyway.
EXCLUDED_DEFINES = re.compile(r'^(NUE_VERSION_(MAJOR|MINOR|PATCH)|NUE_\w*API_VERSION)$')   # the version itself is recorded separately
NUMERIC_BODY = re.compile(r'^[\s\(\)0-9a-fA-FxXuUlL<>|&+\-~*/]*$')
KEYWORDS_NOT_TYPES = {'typedef', 'static', 'extern', 'inline', 'const', 'volatile', 'struct', 'union', 'enum', 'unsigned', 'signed'}


def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def logical_lines(text):
    return text.replace('\\\n', ' ').split('\n')


def scan_header(path):
    raw = open(path, encoding='utf-8').read().replace('\r\n', '\n')
    text = strip_comments(raw)
    defines = []
    body = []
    cond_depth = 0   # #if nesting; the include guard is depth 1, anything deeper is platform/compiler dependent and not part of the table
    for ln in logical_lines(text):
        s = ln.strip()
        if s.startswith('#'):
            if re.match(r'#\s*if', s):
                cond_depth += 1
            elif re.match(r'#\s*endif', s):
                cond_depth -= 1
            m = re.match(r'#\s*define\s+(NUE_\w+)(?!\()\s+(.*)$', s)
            if m and cond_depth <= 1 and not re.match(r'#\s*define\s+NUE_\w+\(', s):
                name, value = m.group(1), m.group(2).strip()
                if value and NUMERIC_BODY.match(value) and re.search(r'\d', value) and not EXCLUDED_DEFINES.match(name):
                    defines.append(name)
            continue
        body.append(ln)
    code = '\n'.join(body)
    code = re.sub(r'extern\s+"C"\s*\{', '', code)
    return code, defines


TOKEN = re.compile(r'\s*([A-Za-z_]\w*|\d[\w.]*|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|\.\.\.|->|<<|>>|[{}()\[\];,*=:<>+\-/%&|^~!?.#])')


def tokenize(code):
    toks = []
    pos = 0
    while pos < len(code):
        m = TOKEN.match(code, pos)
        if not m:
            if code[pos:].strip() == '':
                break
            pos += 1
            continue
        toks.append(m.group(1))
        pos = m.end()
    return toks


class Parser:
    def __init__(self, toks):
        self.t = toks
        self.i = 0
        self.structs = []     # (name, [member paths])
        self.enums = []       # enumerator names
        self.funcs = []

    def match_brace(self, i):
        depth = 0
        while i < len(self.t):
            if self.t[i] == '{':
                depth += 1
            elif self.t[i] == '}':
                depth -= 1
                if depth == 0:
                    return i
            i += 1
        return len(self.t) - 1

    def parse_enum_body(self, start, end):
        names = []
        i = start
        expect_name = True
        depth = 0
        while i < end:
            tok = self.t[i]
            if tok in '([{':
                depth += 1
            elif tok in ')]}':
                depth -= 1
            elif depth == 0 and tok == ',':
                expect_name = True
            elif depth == 0 and expect_name and re.match(r'[A-Za-z_]\w*$', tok):
                names.append(tok)
                expect_name = False
            i += 1
        return names

    def parse_members(self, start, end, prefix):
        """Members between start and end (exclusive) of a struct/union body: list of dotted member paths (nested named members expanded)."""
        paths = []
        i = start
        while i < end:
            # one declaration: tokens up to ';' at depth 0
            j = i
            depth = 0
            nested = None
            while j < end:
                tk = self.t[j]
                if tk in '([':
                    depth += 1
                elif tk in ')]':
                    depth -= 1
                elif tk == '{' and depth == 0:
                    close = self.match_brace(j)
                    nested = (j, close)
                    j = close
                elif tk == ';' and depth == 0:
                    break
                j += 1
            decl = self.t[i:j]
            if not decl:
                i = j + 1
                continue
            if ':' in decl and nested is None and '?' not in decl:
                # bit field: its offset cannot be taken; the struct size still covers it
                i = j + 1
                continue
            if nested is not None:
                a, b = nested
                head = decl[:a - i]
                tail = decl[b - i + 1:]
                is_type = any(h in ('struct', 'union') for h in head)
                names = self.declarator_names(tail)
                inner_start, inner_end = a + 1, b
                if is_type and names:
                    for nm in names:
                        paths.append(prefix + nm)
                        paths.extend(self.parse_members(inner_start, inner_end, prefix + nm + '.'))
                elif is_type:
                    # anonymous struct/union member: members belong to the parent
                    paths.extend(self.parse_members(inner_start, inner_end, prefix))
                else:
                    # enum { ... } field; ...
                    for nm in names:
                        paths.append(prefix + nm)
            else:
                for nm in self.declarator_names(decl, with_type=True):
                    paths.append(prefix + nm)
            i = j + 1
        return paths

    def declarator_names(self, toks, with_type=False):
        """Names declared by `type a, *b, c[4], (*fn)(args)` (with_type) or `a, b[2]` (a declarator list only)."""
        names = []
        # split by commas at depth 0
        parts = []
        cur = []
        depth = 0
        for tk in toks:
            if tk in '([':
                depth += 1
            elif tk in ')]':
                depth -= 1
            if tk == ',' and depth == 0:
                parts.append(cur)
                cur = []
            else:
                cur.append(tk)
        parts.append(cur)
        for k, part in enumerate(parts):
            if not part:
                continue
            # function pointer: ( [CALLCONV] * name ) (
            for idx in range(len(part) - 2):
                if part[idx] == '(' and '*' in part[idx + 1:idx + 4]:
                    m = None
                    for q in range(idx + 1, min(idx + 6, len(part))):
                        if part[q] == ')':
                            break
                        if re.match(r'[A-Za-z_]\w*$', part[q]) and part[q] not in ('NUE_CALL', 'NUE_API'):
                            m = part[q]
                    if m:
                        names.append(m)
                        break
            else:
                # plain declarator: the last identifier before an optional [..] suffix
                depth = 0
                last = None
                for tk in part:
                    if tk == '[':
                        break
                    if re.match(r'[A-Za-z_]\w*$', tk):
                        last = tk
                if last and (k > 0 or not with_type or len(part) >= 2):
                    names.append(last)
        return names

    def run(self):
        t = self.t
        n = len(t)
        i = 0
        while i < n:
            # a top-level statement
            j = i
            depth = 0
            block = None
            while j < n:
                tk = t[j]
                if tk in '([':
                    depth += 1
                elif tk in ')]':
                    depth -= 1
                elif tk == '{' and depth == 0:
                    block = (j, self.match_brace(j))
                    j = block[1]
                    if t[i] != 'typedef' and t[i] not in ('struct', 'enum', 'union') and not (t[i] == 'static' and False):
                        # a function body: statement ends at the closing brace
                        break
                elif tk == ';' and depth == 0:
                    break
                j += 1
            stmt = t[i:j + 1]
            if stmt and stmt[0] != ';':
                self.handle(stmt, block, i)
            i = j + 1

    def handle(self, stmt, block, base):
        if stmt[0] in ('static', 'inline') and block:
            return
        is_typedef = stmt[0] == 'typedef'
        if block:
            a, b = block
            head = self.t[base:a]
            kind = next((h for h in head if h in ('struct', 'union', 'enum')), None)
            tail = self.t[b + 1:base + len(stmt)]
            if kind == 'enum':
                self.enums.extend(self.parse_enum_body(a + 1, b))
                return
            if kind in ('struct', 'union'):
                names = [x for x in tail if re.match(r'[A-Za-z_]\w*$', x)]
                if is_typedef and names:
                    self.structs.append((names[-1], self.parse_members(a + 1, b, '')))
                elif not is_typedef:
                    tag = [h for h in head if re.match(r'[A-Za-z_]\w*$', h) and h not in ('struct', 'union')]
                    if tag:
                        self.structs.append((tag[-1], self.parse_members(a + 1, b, '')))
                return
            return
        # function declaration: ... NUE_API ... name ( ...
        if 'NUE_API' in stmt or any(x.startswith('NUE_') and x.endswith('_API') for x in stmt):
            for idx, tk in enumerate(stmt):
                if tk == '(' and idx > 0 and re.match(r'[A-Za-z_]\w*$', stmt[idx - 1]) and stmt[idx - 1] not in ('NUE_CALL', 'NUE_API'):
                    # skip a '(' that belongs to the calling convention macro or a function pointer return
                    self.funcs.append(stmt[idx - 1])
                    break


# Module headers carry their own API version; everything else belongs to the core API (NUE_API_VERSION).
GROUPS = {'texture.h': ('texture', 'NUE_TEX_API_VERSION'), 'assets.h': ('assets', 'NUE_ASSETS_API_VERSION')}
CORE_GROUP = ('core', 'NUE_API_VERSION')


def main():
    inc_dir, out_path = sys.argv[1], sys.argv[2]
    headers = sorted(f for f in os.listdir(os.path.join(inc_dir, 'nue')) if f.endswith('.h'))
    per_header = []
    seen_struct, seen_name = set(), set()
    for h in headers:
        code, defines = scan_header(os.path.join(inc_dir, 'nue', h))
        p = Parser(tokenize(code))
        p.run()
        structs = [s for s in p.structs if s[0] not in seen_struct and not seen_struct.add(s[0])]
        enums = [e for e in p.enums if e not in seen_name and not seen_name.add(e)]
        funcs = [f for f in p.funcs if f not in seen_name and not seen_name.add(f)]
        defs = [d for d in defines if d not in seen_name and not seen_name.add(d)]
        per_header.append((h, structs, enums, defs, funcs))
    lines = ['// GENERATED by tools/abi/gen_abi_probe.py from include/nue/*.h - do not edit']
    for h in headers:
        lines.append('#include "nue/%s"' % h)
    lines.append('')
    lines.append('static void probe_all(AbiTable& t) {')
    counts = [0, 0, 0, 0]
    for h, structs, enums, defs, funcs in per_header:
        group, version_macro = GROUPS.get(h, CORE_GROUP)
        lines.append('    t.set_group("%s", (unsigned long long)(%s));   // %s' % (group, version_macro, h))
        for name, members in structs:
            lines.append('    t.add_struct("%s", sizeof(%s), alignof(%s));' % (name, name, name))
            for m in members:
                lines.append('    t.add_field("%s", "%s", offsetof(%s, %s), sizeof(((%s*)0)->%s));' % (name, m, name, m, name, m))
        for e in enums:
            lines.append('    t.add_enum("%s", (long long)(%s));' % (e, e))
        for d in defs:
            lines.append('    t.add_define("%s", (unsigned long long)(%s));' % (d, d))
        for f in funcs:
            lines.append('    t.add_function("%s");' % f)
        counts[0] += len(structs); counts[1] += len(enums); counts[2] += len(defs); counts[3] += len(funcs)
    lines.append('}')
    text = '\n'.join(lines) + '\n'
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    if not os.path.exists(out_path) or open(out_path, encoding='utf-8').read() != text:
        open(out_path, 'w', encoding='utf-8', newline='\n').write(text)
    sys.stderr.write('abi probe: %d structs, %d enumerators, %d defines, %d functions\n' % tuple(counts))


if __name__ == '__main__':
    main()
