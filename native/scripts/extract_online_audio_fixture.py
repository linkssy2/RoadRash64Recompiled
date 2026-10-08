"""Extract actual native gain/producers and an unhooked negative control.

Normal use requires hooks in generated code. --candidate-config permits a
pre-generation offline test by inserting the exact configured hook statements.
Neither mode launches the game or copies ROM data into the fixture.
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
names = ['func_80059324', 'func_800593F0', 'func_800594BC', 'func_80059588',
         'func_80059648', 'func_8001295C', 'func_8001A250', 'func_8001A424',
         'func_800571DC', 'func_800597A0', 'func_8005656C', 'func_80056208',
         'func_80056000', 'func_80058600', 'func_8004090C', 'func_800645E4']
functions = {}
switch_callers = set()
for path in a.generated.glob('funcs_*.c'):
    for m in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)', path.read_text(), re.S):
        if 'func_80056000(rdram, ctx);' in m[0]:
            switch_callers.add(m[1])
        if m[1] in names:
            functions[m[1]] = m[0]
assert len(functions) == len(names)
assert switch_callers == {'func_8004090C', 'func_800645E4'}, 'Weapon-switch caller ownership changed'
# Preserve actual new rival hook calls while this suite supplies inactive
# scope stubs below; the dedicated rival fixture verifies their active scope.
for line in a.config.read_text().splitlines():
    if 'rr64_rival_engine_' not in line or line.lstrip().startswith('#'):
        continue
    m = re.fullmatch(r'\s*\{\s*func\s*=\s*"(\w+)"\s*,\s*(?:before_vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*)?text\s*=\s*(".*")\s*\},?\s*', line)
    assert m, ('Audit rival hook format', line)
    if m[1] not in functions:
        continue
    address = int(m[2],16) if m[2] else int(m[1][5:],16)
    statement = json.loads(m[3])
    marker = f'    // 0x{address:08X}:'
    code = functions[m[1]]
    assert code.count(marker) == 1
    if not code.split(marker)[0].rstrip().endswith(statement):
        assert a.candidate_config, ('Production rival hook missing', m[1], address)
        code = code.replace(marker, '    ' + statement + '\n' + marker)
    functions[m[1]] = code
# The shipped Linux build host uses Python 3.10 without tomllib. These audited
# hooks are single-line inline tables with literal strings and addresses. Parse
# only that exact form, rejecting format changes instead of guessing TOML.
hook_pattern = re.compile(r'\s*\{\s*func\s*=\s*"(func_[0-9A-F]{8})"\s*,\s*'
                          r'before_vram\s*=\s*(0x[0-9A-F]{8})\s*,\s*'
                          r'text\s*=\s*(".*")\s*\},?\s*')
hooks = []
hud_notifications = 0
for line in a.config.read_text().splitlines():
    if 'rr64_online_audio_' not in line or line.lstrip().startswith('#'):
        continue
    match = hook_pattern.fullmatch(line)
    assert match, ('Audio hook format changed: audit the native fixture', line)
    statement = json.loads(match[3])
    if 'rr64_course_items_weapon_switched' in statement:
        expected = {'func_8004090C': (0x80040F80, 4), 'func_800645E4': (0x80064E98, 16)}
        assert match[1] in expected and int(match[2],16) == expected[match[1]][0]
        register = expected[match[1]][1]
        prefix = ('\n#ifdef RR64_EXPERIMENTAL_COURSE\n'
                  f'rr64_course_items_weapon_switched(rdram,(unsigned)ctx->r{register});\n#endif\n')
        assert statement.startswith(prefix), 'Audit weapon display notification'
        statement = statement[len(prefix):]
        hud_notifications += 1
    assert re.fullmatch(r'rr64_online_audio_(?:owner|listener|weapon_gain|weapon_source)'
                        r'\(rdram, ctx(?:, \(unsigned\)ctx->r(?:4|16))?\);', statement)
    hooks.append({'func': match[1], 'before_vram': int(match[2], 16), 'text': statement})
assert hud_notifications in (0, 2), 'Both weapon display notifications must be audited'
assert len(hooks) == 16, 'Audio hook coverage changed: audit the native fixture'
for h in hooks:
    code = functions[h['func']]
    marker = f"    // 0x{h['before_vram']:08X}:"
    assert code.count(marker) == 1, (h, 'ambiguous instruction marker')
    prefix = code.split(marker)[0]
    if not prefix.rstrip().endswith(h['text']):
        assert a.candidate_config, (h, 'production hook missing')
        code = code.replace(marker, '    ' + h['text'] + '\n' + marker)
    functions[h['func']] = code
# Run the actual two call-site instruction sequences, including their delay
# slots, without simulating unrelated movement/combat earlier in those frames.
for name, start, end, alias in [
        ('func_8004090C', '80040F80', '80040F88', 'audio_switch_from_bike'),
        ('func_800645E4', '80064E98', '80064EA0', 'audio_switch_from_rider')]:
    code = functions.pop(name)
    assert code.count('L_' + start + ':') == code.count('L_' + end + ':') == 1
    body = code.split('L_' + start + ':', 1)[1].split('L_' + end + ':', 1)[0]
    functions[alias] = 'RECOMP_FUNC void ' + alias + '(uint8_t* rdram, recomp_context* ctx) {\n' + body + '\nreturn;\n}\n'
    names[names.index(name)] = alias
# GCC and Clang require generated switch temporaries before goto labels.
# Keep assignments at the original instructions; MSVC retains its source.
for name, code in functions.items():
    temporaries = re.findall(r'    gpr (jr_addend_[0-9A-F]+) = ([^;]+);', code)
    if temporaries:
        opening = code.index('{') + 1
        declarations = '\n#if defined(__GNUC__)\n'
        declarations += ''.join('    gpr ' + n + ';\n' for n, _ in temporaries)
        code = code[:opening] + declarations + '#endif\n' + code[opening:]
        for n, expression in temporaries:
            original = '    gpr ' + n + ' = ' + expression + ';'
            code = code.replace(original, '#if defined(__GNUC__)\n    ' + n + ' = ' + expression + ';\n#else\n' + original + '\n#endif')
        functions[name] = code
stock = []
for name in names:
    code = functions[name]
    for h in hooks:
        code = code.replace(h['text'], '')
    for n in names:
        code = re.sub(r'\b' + n + r'\b', 'stock_' + n, code)
    stock.append(code)
calls = set(re.findall(r'^\s+(\w+)\(rdram, ctx\);', ''.join(functions.values()), re.M))
calls = {n for n in calls if not n.startswith(('rr64_online_audio_', 'rr64_rival_engine_'))}
decls = '\n'.join(f'void {n}(unsigned char*,recomp_context*);' for n in sorted(calls | set(names) | {'stock_' + n for n in names}))
decls += '\nvoid rr64_online_audio_owner(unsigned char*,void*);\nvoid rr64_online_audio_listener(unsigned char*,void*);'
a.output.parent.mkdir(parents=True, exist_ok=True)
# This suite isolates existing human cue ownership. Rival producer/allocation
# hooks run in RR64RivalEngineSmoke with their actual implementation. Keep the
# generated hook calls here and provide explicit inactive-scope substitutes.
rival_header = a.config.parent.parent / 'native/src/rr64_rival_engine.hpp'
rival_stubs = ''
if rival_header.exists():
    for result, name, arguments in re.findall(r'^(void|int|unsigned) (rr64_rival_engine_\w+)\(([^;]*)\);', rival_header.read_text(), re.M):
        if name == 'rr64_rival_engine_effect_limit':
            rival_stubs += 'unsigned rr64_rival_engine_effect_limit(unsigned count) { return count; }\n'
        else:
            rival_stubs += result + ' ' + name + '(' + arguments + ') {' + ('return 0;' if result == 'int' else '') + '}\n'
# HUD focus is independently tested with the complete native cycle and sprite
# consumer. Here it has no sound service and must not change source ownership.
if hud_notifications:
    rival_stubs += 'void rr64_course_items_weapon_switched(unsigned char*,unsigned) {}\n'
a.output.write_text('#include "recomp.h"\n#include "rr64_native.hpp"\nextern "C" {\n' + decls + '\n' + rival_stubs + ''.join(functions[n] for n in names) + ''.join(stock) + '}\n')
print(f'Extracted 14 native functions and 2 native caller sequences, {len(hooks)} hooks; candidate injection allowed={a.candidate_config}')
print('Modeled external calls:', sorted(calls - set(names)))
