"""Execute the generated damage coefficients and force cap/multiplier branches."""
from pathlib import Path
import sys

generated, output = map(Path, sys.argv[1:])
code = '#include "recomp.h"\n#include "rr64_offline_modifiers.hpp"\n'
for name, file, begin, end in [
    ('damage_left', 'funcs_15.c', '800606D8', '80060730'),
    ('damage_right', 'funcs_15.c', '8006083C', '80060898'),
    ('force', 'funcs_16.c', '80065ED0', '80065F4C'),
]:
    source = (generated / file).read_text()
    start, stop = f'    // 0x{begin}:', f'    // 0x{end}:'
    body = start + source.split(start, 1)[1].split(stop, 1)[0]
    assert body.count('rr64_offline_combat_multiplier(') == 1, name
    code += f'extern "C" void test_combat_{name}(unsigned char *rdram, recomp_context *ctx) {{\nint c1cs=0;\n{body}\nreturn;\n}}\n'
for name, file, begin, end in [
    ('rider_special', 'funcs_7.c', '80036980', '80036994'),
    ('rider', 'funcs_7.c', '8003699C', '800369B4'),
    ('bike_special', 'funcs_9.c', '800459D0', '800459EC'),
    ('bike_blend', 'funcs_9.c', '800459F4', '80045A2C'),
    ('bike', 'funcs_9.c', '80045A34', '80045A54'),
]:
    source = (generated / file).read_text()
    start, stop = f'    // 0x{begin}:', f'    // 0x{end}:'
    body = start + source.split(start, 1)[1].split(stop, 1)[0]
    assert body.count('rr64_offline_gravity(') == 1, name
    code += f'extern "C" void test_gravity_{name}(unsigned char *rdram, recomp_context *ctx) {{\n{body}\n}}\n'
output.write_text(code)
