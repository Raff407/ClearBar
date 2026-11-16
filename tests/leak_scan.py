"""Scan files (binaries included) for strings that could identify the builder.

Usage: python -I tests/leak_scan.py <file-or-dir>... [--extra WORD ...]
Exits 1 if anything suspicious is found. Checks ASCII and UTF-16LE strings for
user-profile paths, .pdb paths, e-mail addresses, machine/user names from the
environment, and any --extra words.
"""
import os
import re
import sys

args = sys.argv[1:]
extra = []
if '--extra' in args:
    i = args.index('--extra')
    extra = args[i + 1:]
    args = args[:i]

needles = {w.lower() for w in extra if w}
for var in ('USERNAME', 'COMPUTERNAME', 'USERDOMAIN'):
    v = os.environ.get(var)
    if v and len(v) >= 3:
        needles.add(v.lower())

patterns = [
    re.compile(rb'[A-Za-z]:\\Users\\', re.I),
    re.compile(rb'\\AppData\\', re.I),
    re.compile(rb'\.pdb\b', re.I),
    re.compile(rb'[\w.+-]+@[\w-]+\.[\w.]+'),
]


def strings(data: bytes):
    for m in re.finditer(rb'[\x20-\x7e]{4,}', data):
        yield m.group()
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', data):
        yield m.group().replace(b'\x00', b'')


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, fs in os.walk(p):
                dirs[:] = [d for d in dirs if d not in ('.git', 'obj')]
                for f in fs:
                    yield os.path.join(root, f)
        else:
            yield p


# Known-harmless matches (public URLs / license text).
allow = [rb'licensing@fsf.org', rb'noreply.github.com', rb'%LOCALAPPDATA%', rb'$env:LOCALAPPDATA']

hits = 0
for path in files(args):
    with open(path, 'rb') as fh:
        data = fh.read()
    for s in strings(data):
        low = s.lower()
        bad = [p.pattern.decode() for p in patterns if p.search(s)]
        bad += [n for n in needles if n.encode() in low]
        if bad and not any(a.lower() in low for a in allow):
            hits += 1
            print(f'{path}: {s[:160]!r}  <- {bad}')
print(f'{hits} suspicious string(s)')
sys.exit(1 if hits else 0)
