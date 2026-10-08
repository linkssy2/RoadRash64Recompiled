"""Extract native static broadphase and collision math, without ROM assets.

Only crash/audio side effects and visibility admission are test boundaries.
The original geometric contacts and force calculations remain executable.
"""
from pathlib import Path
import argparse
import json
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--recompiled-dir', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
functions = {}
for source in sorted(a.recompiled_dir.glob('funcs_*.c')):
    for m in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)', source.read_text(), re.S):
        functions[m[1]] = m[0]
boundaries = {'func_80056B04', 'func_80056BA8', 'func_80056DA8',
              'n_alSeqpDelete_copy_80056F10', 'n_alSeqpDelete_copy_80056F2C',
              'func_800565BC', 'func_80061BBC', 'func_800784B4',
              'func_80078588', 'func_8004E754', 'rr64_highlight_camera_floor_cell',
              'rr64_experimental_course_floor_indices', 'rr64_experimental_course_floor_subindices'}
production = {'rr64_online_terrain_collision_begin',
              'rr64_online_terrain_collision_objects',
              'rr64_online_terrain_collision_walls'}
floor_hooks = {'rr64_online_terrain_query_begin', 'rr64_online_terrain_lookup'}
todo = ['func_80062594', 'func_8001BD50', 'func_8001BDF8', 'func_80034370',
        'func_80014604', 'func_80014DE4']
seen = set()
while todo:
    name = todo.pop()
    if name in seen or name in boundaries or name in production or name in floor_hooks:
        continue
    if name not in functions:
        raise ValueError('Unreviewed external collision dependency: ' + name)
    seen.add(name)
    todo.extend(re.findall(r'\b(\w+)\(rdram\s*,', functions[name]))
parts = ['#include "recomp.h"', '#include "rr64_native.hpp"', '#include "funcs.h"']
# Run exactly the same generated branch math with and without the maintained
# fallback. The negative control cannot accidentally use a second algorithm.
base = functions['func_80062594']
if any(base.count(name + '(') != 1 for name in production):
    raise ValueError('Regenerate native62594: expected each current collision hook exactly once')
negative = re.sub(r'^.*rr64_online_terrain_collision_.*\n', '', base, flags=re.M)
negative = negative.replace('func_80062594(', 'rr64_static_collision_legacy(', 1)
parts.extend(functions[name] for name in sorted(seen))
parts.append(negative)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text('\n'.join(parts), encoding='utf-8')
a.output.with_suffix('.json').write_text(json.dumps({
    'nativeFunctions': sorted(seen), 'testBoundaries': sorted(boundaries),
    'productionHooks': sorted(production | floor_hooks),
    'negativeControl': 'same62594 with only immutable collision hooks omitted',
    'assetsEmbedded': False,
}, indent=2)+'\n')
