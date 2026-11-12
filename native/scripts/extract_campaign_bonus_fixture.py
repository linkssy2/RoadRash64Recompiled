"""Extract actual campaign consumers without rendering, devices, or ROM assets.

Normal extraction requires generated production hook placement. The explicit
candidate option inserts the proposed config hooks before a full regeneration.
"""
from pathlib import Path
import argparse
import json
import re

p = argparse.ArgumentParser()
p.add_argument('generated', type=Path)
p.add_argument('output', type=Path)
p.add_argument('config', type=Path)
p.add_argument('--candidate-config', action='store_true')
a = p.parse_args()
all_functions = {}
for path in a.generated.glob('funcs_*.c'):
    for m in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)', path.read_text(), re.S):
        all_functions[m[1]] = m[0]
hooks = []
for line in a.config.read_text().splitlines():
    if not any(h in line for h in ('rr64_campaign_', 'rr64_coop_', 'rr64_ai_bike_profile')) or line.lstrip().startswith('#'):
        continue
    m = re.fullmatch(r'\s*\{\s*func\s*=\s*"(\w+)"\s*,\s*(?:before_vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*)?text\s*=\s*(".*")\s*\},?\s*', line)
    assert m, ('Audit changed campaign hook format', line)
    h = dict(func=m[1], address=int(m[2], 16) if m[2] else int(m[1][5:], 16), text=json.loads(m[3]))
    hooks.append(h)
    code = all_functions[h['func']]
    marker = f"    // 0x{h['address']:08X}:"
    assert marker in code, h
    # Delay slots can have duplicated instruction bodies in generated C.
    pieces = code.split(marker)
    for i in range(len(pieces)-1):
        if not pieces[i].rstrip().endswith(h['text']):
            assert a.candidate_config, ('Missing generated campaign hook', h)
            pieces[i] += '    ' + h['text'] + '\n'
    all_functions[h['func']] = marker.join(pieces)

full = ['func_80073658', 'func_80073728', 'func_80072E74', 'func_80072A14',
        'func_80073000', 'func_8005F420', 'func_8001A250', 'func_800488C4', 'func_80048544',
        'func_800516B8', 'func_8001A288', 'func_8001A2E8',
        'func_8001A500', 'func_8001A52C', 'func_8001A590', 'func_8001A5D8',
        'func_80048484', 'func_80048564', 'func_80074200',
        'func_8005C298', 'func_8005C320', 'func_80012E7C', 'func_80015C10']
functions = {n: all_functions[n] for n in full}

# Reproduce the shipped co-op defect with the same dispatcher and constructor,
# removing only the camera repair while retaining two-human roster allocation.
for name in ('func_80073728', 'func_8005C298'):
    code = functions[name]
    for h in hooks:
        if 'rr64_coop_split_camera' in h['text']:
            code = code.replace(h['text'], '')
    functions['old_coop_'+name] = code.replace('void '+name+'(', 'void old_coop_'+name+'(')

def slice_native(source, name, start, end, tail=''):
    code = all_functions[source]
    # Include hooks belonging to the first instruction, but no preceding code.
    begin = code.index(f'    // 0x{start}:')
    prefix = ''
    for h in hooks:
        if h['func'] == source and h['address'] == int(start, 16):
            prefix += h['text'] + '\n'
    finish = code.index(f'    // 0x{end}:', begin)
    body = prefix + code[begin:finish]
    # A label immediately before the omitted instruction belongs to the tail.
    body = re.sub(r'\n(?:L_[A-F0-9]+:\s*)+$', '\n', body)
    if tail and tail.removesuffix(';') in body:
        tail = ';'
    functions[name] = ('RECOMP_FUNC void '+name+'(uint8_t* rdram,recomp_context* ctx) {\n'
        'uint64_t hi=0,lo=0,result=0; int c1cs=0;\n'+body+'\n'+tail+'\n}\n')

slice_native('func_80029DB8', 'bonus_native_route', '80029F68', '8002A0EC', 'L_8002A0EC:;')
slice_native('func_80073054', 'bonus_native_postrace', '80073218', '80073468', 'L_80073468:;')
slice_native('func_800516B8', 'bonus_native_ai_pool', '80051954', '800519E0', 'L_800519E0:;')
# Run the original confirmed-Yes writes before native pause/abort dispatch;
# menu rendering and input selection are outside this headless fixture.
slice_native('func_8002E5CC', 'bonus_native_quit_yes', '8002E9EC', '8002EA18', ';')
for source, address in [('8002C13C', '8002C3D8'), ('8002C520', '8002C718'),
                        ('800728B8', '8007290C'), ('80072930', '800729B4'),
                        ('8002DC30', '8002DE20'), ('8002DC30', '8002DE4C'),
                        ('8002DC30', '8002DE84'), ('80073940', '80073994'),
                        ('800739E8', '80073A34')]:
    slice_native('func_'+source, 'bonus_load_'+address, address, f'{int(address,16)+4:08X}')

# Stock counterparts independently establish that chapters 0..4 are unchanged.
stock = {}
for name in ['bonus_native_route', 'func_80073658', 'func_80072E74', 'bonus_native_postrace']:
    code = functions[name]
    for h in hooks:
        code = code.replace(h['text'], '')
    code = code.replace('void '+name+'(', 'void stock_'+name+'(')
    stock['stock_'+name] = code
functions.update(stock)

# Negative control keeps the shipped producer, reservation and demand repair,
# but omits the new model randomization at the actual allocation call site.
selection = 'ctx->r5 = rr64_ai_bike_profile(rdram, ctx, ctx->r5);'
assert functions['func_800516B8'].count(selection) == 1, 'Missing AI selection call site'
functions['stock_func_800516B8'] = functions['func_800516B8'].replace(selection, '').replace(
    'void func_800516B8(', 'void stock_func_800516B8(')

# C++ rejects jumps across generated jump-table temporary initialization.
for name, code in functions.items():
    variables = re.findall(r'    gpr (jr_addend_\w+) = ', code)
    for variable in variables:
        code = code.replace('    gpr '+variable+' = ', '    '+variable+' = ')
    if variables:
        point = code.index('{') + 1
        code = code[:point] + '\n' + ''.join('gpr '+v+'=0;\n' for v in variables) + code[point:]
    functions[name] = code
calls = set(re.findall(r'^\s+(\w+)\(rdram, ctx\);', ''.join(functions.values()), re.M))
decls = '\n'.join('void '+n+'(unsigned char*,recomp_context*);'
    for n in sorted(calls | set(functions)) if not n.startswith('rr64_'))
a.output.parent.mkdir(parents=True, exist_ok=True)
normalizer_source=(a.config.parent.parent/'native/src/rr64_local_race_options.cpp').read_text()
normalizer=re.search(r'extern "C" void rr64_local_bike_ai_pool\([^\n]+\) \{.*?\n\}',normalizer_source,re.S)
assert normalizer, 'Missing production AI demand normalizer'
# Extract the exact production helper. Dependencies represent an offline
# campaign with no Thrash/local/Custom-Cop override, not its algorithm.
support='''
#include <array>
#include "rr64_engine_layout.hpp"
#include "rr64_netplay.hpp"
namespace {
using namespace rr64::engine;
unsigned race_bike_choice=0;
bool local() { return false; }
unsigned read(unsigned char* m,unsigned a) { unsigned v=0; read_u32(m,a,v); return v; }
void write(unsigned char* m,unsigned a,unsigned v) { write_u32(m,a,v); }
}
'''
a.output.write_text('#include "recomp.h"\n#include "rr64_native.hpp"\n#include "rr64_ai_bike_selection.hpp"\n'+support+normalizer[0]+'\n'
    '#undef RECOMP_FUNC\n#define RECOMP_FUNC\nextern "C" {\n'+decls+'\n'+''.join(functions.values())+'}\n', encoding='utf-8')
print(json.dumps(dict(functions=list(functions), campaignHooks=len(hooks), candidateInjection=a.candidate_config,
    externalCalls=sorted(calls-set(functions)))))
