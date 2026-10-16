"""Extract the original option input dispatchers with their production hooks."""
from pathlib import Path
import hashlib
import json
import re
import sys

source, output = map(Path, sys.argv[1:3])
text = (source / "funcs_7.c").read_text()
parts = ['#include "recomp.h"\n#include "rr64_race_pack_menu.hpp"\n']
for name, start, end, multiplayer in [
    ("solo", "80025914", "80025C38", 0),
    ("multiplayer", "800285C4", "8002883C", 1),
]:
    hook = f"if (rr64_race_pack_menu_options_input(rdram, {multiplayer})) goto L_{end};"
    begin = text.index(hook)
    finish = text.index(f"L_{end}:", begin)
    body = text[begin:finish]
    assert f"// 0x{start}:" in body
    assert not re.findall(r"\bfunc_[0-9A-Fa-f]+\(", body)
    labels = set(re.findall(r"^(L_[0-9A-F]+):", body, re.M))
    exits = sorted(set(re.findall(r"goto (L_[0-9A-F]+);", body)) - labels)
    prefix = "ctx->r13 = S32(0X800A << 16);\n" if multiplayer else ""
    parts.append(f"void item_options_native_{name}(unsigned char *rdram, recomp_context *ctx) {{\n"
                 + prefix + body + "\n" + "\n".join(label + ":" for label in exits)
                 + "\nreturn;\n}\n")
output.parent.mkdir(parents=True, exist_ok=True)
finish_source = (source / "funcs_19.c").read_text()
begin = finish_source.index("RECOMP_FUNC void func_800740A0(")
end = finish_source.index("RECOMP_FUNC void ", begin + 1)
parts.append('#include "rr64_custom_cop.hpp"\n' + finish_source[begin:end])
output.write_text("\n".join(parts))
output.with_suffix(".json").write_text(json.dumps({
    "source": str(source / "funcs_7.c"),
    "source_sha256": hashlib.sha256(text.encode()).hexdigest(),
    "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
    "ranges": ["80025914..80025C38", "800285C4..8002883C"],
    "generated_hooks_required": True,
    "game_launched": False,
}, indent=2))
