#!/usr/bin/env python3
"""Extracts one shell function from a BitBake recipe or include file into a standalone bash script, so the Docker tests can run the recipe's own
code with a stub bbfatal. Usage: extract_bb_func.py <file.bb|inc> <function> <out.sh> [NAME=value ...]
Every ${NAME} that has a NAME=value argument is replaced by the value (BitBake would expand it); every other ${...} stays and is expanded by bash
from the environment of the caller (as the bundle recipe's checks expect)."""
import re
import sys

src, func, out = sys.argv[1:4]
text = open(src).read()
m = re.search(r"^" + re.escape(func) + r"\(\) \{\n(.*?)^\}\n", text, re.S | re.M)
if not m:
    sys.exit("function %s not found in %s" % (func, src))
body = m.group(1)
for kv in sys.argv[4:]:
    k, v = kv.split("=", 1)
    body = body.replace("${%s}" % k, v)
open(out, "w").write("bbfatal() { echo \"FATAL: $*\"; exit 1; }\n" + body)
