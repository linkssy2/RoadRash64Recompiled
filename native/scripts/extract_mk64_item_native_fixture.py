"""Compile the current native crash/contact boundaries without launching a game.

The 36B78 prefix ends before its detached-body integration; complete 37554,
373F0, contact-sphere and impulse consumers remain original generated code.
Only renderer/statistics/audio IO and collision callees not reached by a Boo
early return are stubbed. Strict mode requires the production config hooks.
"""
from pathlib import Path
import argparse
import hashlib
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
    for m in re.finditer(r'RECOMP_FUNC void func_([A-F0-9]+)\(.*?(?=\nRECOMP_FUNC |\Z)', path.read_text(), re.S):
        all_functions[m[1]] = m[0]

roots = {'80036B78', '80037554', '8004E754', '80034594', '80034370',
         '800348D8', '80048FA4', '80061224', '800616BC', '8004918C',
         '80015A90', '80012CE0', '80012DBC'}
stub = {'8001A5D8', '80055184', '80055224', '80056330', '80063B14', '80063B50', '800642A8'}
selected = {}
pending = list(roots)
while pending:
    name = pending.pop()
    if name in selected or name in stub:
        continue
    code = all_functions[name]
    if name == '80036B78':
        code = code.split('    // 0x80036E20:')[0] + 'L_80036F60:\nreturn;\n}\n'
    selected[name] = code
    pending += re.findall(r'func_([A-F0-9]+)\(rdram, ctx\)', code)

# These complete functions are tested only for a Boo early return. Their
# external contact callbacks trap if unexpectedly reached in that case.
for name in ['8005F7A4', '8005FA18', '80060370']:
    selected[name] = all_functions[name]

drivetrain = all_functions['8003A000']
drivetrain = drivetrain[drivetrain.index('    // 0x8003A098:'):drivetrain.index('    // 0x8003A0C4:')]
selected['item_native_rpm'] = ('RECOMP_FUNC void item_native_rpm(uint8_t*rdram,recomp_context*ctx) {\n' +
    drivetrain + '\n;\n}\n')

audited = []
for line in a.config.read_text().splitlines():
    if 'rr64_mk64_items_' not in line or line.lstrip().startswith('#'):
        continue
    h = re.fullmatch(r'\s*\{\s*func\s*=\s*"func_(\w+)"\s*,\s*(?:before_vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*)?text\s*=\s*(".*")\s*\},?\s*', line)
    assert h, ('Audit changed hook format', line)
    if h[1] not in selected:
        continue
    hook = json.loads(h[3])
    address = int(h[2], 16) if h[2] else int(h[1], 16)
    marker = f'    // 0x{address:08X}:'
    code = selected[h[1]]
    pieces = code.split(marker)
    assert len(pieces) > 1, (h[1], address)
    for i in range(len(pieces) - 1):
        if not pieces[i].rstrip().endswith(hook.strip()):
            # Replace the prior complete entry hook, if present, so online
            # damage forwarding is not called twice in candidate-only tests.
            old = re.search(r'\n    (if \(rr64_online_hit[^\n]+)\n$', pieces[i])
            if old:
                pieces[i] = pieces[i][:old.start()] + '\n'
            assert a.candidate_config, ('Missing generated hook', h[1], hex(address))
            pieces[i] += hook + '\n'
    selected[h[1]] = marker.join(pieces)
    audited.append({'function': h[1], 'address': hex(address), 'hook': hook})

# Exact original bike contact call/branch plus the production callback. Stop
# before normal pair response, which has separate original physics consumers.
contact = selected['8005F7A4']
contact = contact[contact.index('    // 0x8005F8AC:'):contact.index('    // 0x8005F8CC:')]
selected['item_native_bike_contact'] = (
    'RECOMP_FUNC void item_native_bike_contact(uint8_t*rdram,recomp_context*ctx) {\n'
    + contact + '\nL_8005F9EC:\nreturn;\n}\n')

# Preserve both native inventory-cycle branches, their empty-slot/wrap logic,
# completed-selection hook and original sound-call delay slot. Rendering the
# selected icon is covered by the linked original HUD queue fixture.
cycle_hooks = {}
for line in a.config.read_text().splitlines():
    if 'rr64_course_items_weapon_switched' not in line or line.lstrip().startswith('#'):
        continue
    h = re.fullmatch(r'\s*\{\s*func\s*=\s*"func_(\w+)"\s*,\s*before_vram\s*=\s*'
                     r'(0x[0-9A-Fa-f]+)\s*,\s*text\s*=\s*(".*")\s*\},?\s*', line)
    assert h and h[1] in ('8004090C', '800645E4'), 'Audit weapon-cycle hook location'
    cycle_hooks[h[1]] = (int(h[2], 16), json.loads(h[3]))
assert len(cycle_hooks) == 2, 'Both native weapon-cycle paths need HUD notification'
for name, function, begin, end, entry, address in [
        ('mounted', '8004090C', 0x80040F00, 0x80040F88, 0x80040F10, 0x80040F80),
        ('rider', '800645E4', 0x80064E34, 0x80064EA0, 0x80064E40, 0x80064E98)]:
    hook_address, hook = cycle_hooks[function]
    assert hook_address == address
    original = all_functions[function]
    marker = f'    // 0x{address:08X}:'
    before, after = original.split(marker, 1)
    assert hook.strip() in before[-1800:], ('Missing generated cycle hook', function)
    marker = f'    // 0x{begin:08X}:'
    chunk = marker + original.split(marker, 1)[1].split(f'    // 0x{end:08X}:', 1)[0]
    assert hook in chunk and '0X5B0' in chunk and '0X838' in chunk
    labels = '' if f'L_{end:08X}:' in chunk else f'L_{end:08X}:\n'
    if name == 'rider':
        labels += 'L_80064EA4:\n'
    selected['item_native_cycle_' + name] = (
        f'RECOMP_FUNC void item_native_cycle_{name}(uint8_t*rdram,recomp_context*ctx) {{\n'
        f'goto L_{entry:08X};\nL_{begin:08X}:\n' + chunk + '\n' + labels + 'return;\n}\n')
    audited.append({'function': function, 'address': hex(address), 'hook': hook})

for function, address, register in [('8004090C', 0x80040F08, 4), ('800645E4', 0x80064E38, 16)]:
    expected = f'rr64_course_items_weapon_wrapped(rdram,(unsigned)ctx->r{register});'
    matching = [line for line in a.config.read_text().splitlines()
                if f'func_{function}' in line and f'0x{address:08X}' in line and expected in line]
    assert len(matching) == 1, ('Audit exact native wrap hook', function)
    chunk = selected['item_native_cycle_' + ('mounted' if register == 4 else 'rider')]
    assert expected in chunk, ('Missing generated native wrap notification', function)
    audited.append({'function': function, 'address': hex(address), 'hook': expected})

# Execute both complete host input callbacks, including filtering before every
# set_local_input call. Only devices/window/menu/rumble are fixture services.
main_path = Path(__file__).resolve().parents[1] / 'src/main.cpp'
main = main_path.read_text(encoding='utf-8')
local = main[main.index('bool get_local_input_with_road_rumble('):main.index('\nvoid set_gameplay_rumble(')]
callback_start = main.index('bool get_input_with_trace(')
callback = main[callback_start:main.index('\n}\n', callback_start) + 3]
assert local.index('get_n64_input(') < local.index('physical_buttons =') < local.index('filter_cycle_input(') < local.rindex('return got_response;')
assert callback.count('rr64::netplay::set_local_input(') == 4
input_text = '// Complete production local-input and input-publication callbacks.\n' + local + '\n' + callback
a.output.with_name('rr64_mk64_item_native_input_fixture.inc').write_text(input_text, encoding='utf-8')

# Original quantity lookup, signed-count gate, formatting and text submission.
# Only the text formatter/rasterizer endpoints are observed by the fixture.
quantity = all_functions['80030220']
quantity = ('    // 0x800322E0:' + quantity.split('    // 0x800322E0:', 1)[1]
            .split('    // 0x80032388:', 1)[0])
assert 'rr64_course_items_hud_weapon' in quantity and 'rr64_course_items_hud_quantity' in quantity
selected['item_native_quantity'] = (
    'RECOMP_FUNC void item_native_quantity(uint8_t*rdram,recomp_context*ctx) {\n'
    + quantity + '\nreturn;\n}\n')

# Preserve the original camera-relative anchor producer and the exact native
# quaternion-to-matrix call site, including the already-generated scale hook.
# No candidate hook injection is permitted at this renderer boundary.
for name, function, begin, end in [
        ('anchors', '8005D9A4', 0x8005E6D8, 0x8005E738),
        ('matrix', '80011CC0', 0x80011E70, 0x80011E80)]:
    original = all_functions[function]
    marker = f'    // 0x{begin:08X}:'
    chunk = marker + original.split(marker, 1)[1].split(f'    // 0x{end:08X}:', 1)[0]
    if name == 'matrix':
        assert 'rr64_mk64_items_scale_matrix(rdram, ctx->r19, ctx->r18, ctx->r29 + 0x10)' in chunk
    selected['item_native_render_' + name] = (
        f'RECOMP_FUNC void item_native_render_{name}(uint8_t*rdram,recomp_context*ctx) {{\n'
        + chunk + '\nreturn;\n}\n')

calls = set(re.findall(r'func_([A-F0-9]+)\(rdram, ctx\)', ''.join(selected.values())))
external = calls - selected.keys()
code = '#include "recomp.h"\n#include "rr64_native.hpp"\n'
code += 'void _nsqrtf(unsigned char*,recomp_context*);\n'
code += 'void sprintf_recomp(unsigned char*,recomp_context*);\n'
code += 'void fixture_native_stub(unsigned,recomp_context*);\n'
code += ''.join(f'void func_{n}(unsigned char*,recomp_context*);\n' for n in sorted(calls | selected.keys()) if not n.startswith('item_native_'))
code += ''.join(f'void func_{n}(unsigned char*m,recomp_context*c) {{ (void)m; fixture_native_stub(0x{n}u,c); }}\n' for n in sorted(external))
code += '\n'.join(selected.values())
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(code)
# Keep the actual finite-triangle surface sweep in the runtime integration
# fixture. Native wall response hooks below this boundary need the full game;
# the immutable geometry builder and sweep above it have no runtime globals.
walls_path = Path(__file__).resolve().parents[1] / 'src/rr64_course_walls.cpp'
walls = walls_path.read_text()
walls_marker = '#include "rr64_course_hazards.hpp"'
assert walls.count(walls_marker) == 1, 'Audit changed wall runtime boundary'
walls = walls.split(walls_marker)[0]
assert walls.rstrip().endswith('} // namespace rr64::course_walls')
a.output.with_name(a.output.stem + '_walls.cpp').write_text(walls)
a.output.with_suffix('.json').write_text(json.dumps({
    'candidate_hooks': a.candidate_config, 'hooks': audited,
    'functions': {n: hashlib.sha256(s.encode()).hexdigest() for n, s in selected.items()},
    'external_stubs': sorted(external),
    'surface_sweep_sha256': hashlib.sha256(walls.encode()).hexdigest(),
    'main_input_sha256': hashlib.sha256(input_text.encode()).hexdigest(),
    'prefix_only': '80036B78 entry through 80036E1C'}, indent=2) + '\n')

