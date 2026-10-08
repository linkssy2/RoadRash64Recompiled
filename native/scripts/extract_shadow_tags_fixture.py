"""Extract original shadow construction/draw instructions; never embed ROM assets."""
from pathlib import Path
import argparse
import hashlib
import json
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("generated", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
names = {"func_80010420", "func_8000D518", "func_8000F9E8", "func_8000F958",
         "func_8000F7F4", "func_80016A18", "func_80016B84", "func_80016DE8",
         "func_8001AD24", "func_8001ACC8", "func_8001A878", "func_8001A610",
         "func_8001A634"}
functions = {}
for path in a.generated.glob("funcs_*.c"):
    for m in re.finditer(r"RECOMP_FUNC void (\w+)\(.*?(?=\nRECOMP_FUNC |\Z)", path.read_text(), re.S):
        if m[1] in names:
            assert m[1] not in functions
            functions[m[1]] = m[0]
assert functions.keys() == names, names - functions.keys()
for name, filename, first, end in (
    ("fixture_bike_owner", "funcs_8.c", "    // 0x8003EF78:", "    func_8003EBC0(rdram, ctx);"),
    ("fixture_rider_owner", "funcs_7.c", "    // 0x800352F8:", "    // 0x80035330:"),
    ("fixture_route_reset", "funcs_16.c", "    // 0x80068C70:", "    // 0x80068CA0:"),
    ("fixture_actor_slot", "funcs_17.c", "    // 0x8006CC30:", "    // 0x8006CC34:"),
):
    source = (a.generated / filename).read_text()
    start = source.index(first)
    body = source[start:source.index(end, start)]
    functions[name] = f"void {name}(uint8_t *rdram, recomp_context *ctx) {{\n" + body + "\n}\n"
code = "\n".join(functions.values())
assert code.count("rr64_shadow_tags_before(") == 4
assert code.count("rr64_shadow_tags_after(") == 4
calls = set(re.findall(r"^\s+(\w+)\(rdram, ctx\);", code, re.M))
decls = "\n".join(f"void {n}(uint8_t*,recomp_context*);" for n in sorted(calls))
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text('#include "recomp.h"\n#include "rr64_native.hpp"\n' + decls + "\n" + code,
                    encoding="utf-8", newline="\n")
a.output.with_suffix(".json").write_text(json.dumps({
    "generatedFunctionSHA256": {n: hashlib.sha256(v.encode()).hexdigest() for n, v in functions.items()},
    "candidateHooksInjected": False,
    "assetsEmbedded": False,
    "scope": "Original bike/rider owner stores, actor slot store, route reset, ROM asset unpacking, graph construction, four shadow call sites, graph traversal, matrix commands and packed quad emission. Only material state, pose calculation, matrix packing/copy and unrelated weapon, skin-preview and projection hooks are fixture boundaries."
}, indent=2) + "\n")
