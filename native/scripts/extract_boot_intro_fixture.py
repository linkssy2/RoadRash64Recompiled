"""Exercise the generated boot cards without opening a window or shipping ROM data."""
from pathlib import Path
import re, sys
source, output = map(Path, sys.argv[1:])
functions = {}
for file in (source/'funcs_6.c', source/'funcs_18.c'):
    for m in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)', file.read_text(), re.S):
        if m[1] in ('func_80021B0C', 'func_80071F94'):
            functions[m[1]] = m[0]
assert len(functions) == 2
body = '\n'.join(functions.values())
assert body.count('rr64_intro_skip(rdram);') == 1
assert 'ctx->r3 = 600;' not in body and 'ctx->r2 |= (ctx->r2 & 0x8000) >> 3;' not in body
runtime = (source.parents[1]/'native/src/rr64_runtime_shims.cpp').read_text()
helper = runtime.split('extern "C" void rr64_intro_skip(', 1)[1].split('#include "rr64_autotest_control.hpp"', 1)[0]
helper = 'extern "C" void rr64_intro_skip(' + helper
calls = set(re.findall(r'(func_[0-9A-F]+)\(rdram, ctx\)', body))
stubs = []
for name in sorted(calls):
    action = ''
    if name == 'func_80048544': action = 'MEM_W(0, (int32_t)0x800A1814) = ctx->r4;'
    if name == 'func_8001EB90': action = 'image = (unsigned)ctx->r5;'
    stubs.append('void '+name+'(uint8_t* rdram, recomp_context* ctx) {'+action+'}')
output.write_text('#include "recomp.h"\n#include "rr64_engine_layout.hpp"\n#include <array>\n#include <atomic>\n'
    '#undef RECOMP_FUNC\n#define RECOMP_FUNC\nunsigned image;\nstd::atomic<bool> player_started_title_session{false};\n'
    + helper + '\n'.join(stubs)+'\n'+body)
