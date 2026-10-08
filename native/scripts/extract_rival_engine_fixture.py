"""Extract real engine producers, voice allocator and stereo arithmetic.

No samples are written. Tests read metadata from the user's private ROM at run
time. Normal builds require actual generated hook placement; candidate-config
is an explicit pre-generation experiment only.
"""
from pathlib import Path
import argparse
import json
import re

p=argparse.ArgumentParser()
p.add_argument('generated',type=Path)
p.add_argument('output',type=Path)
p.add_argument('config',type=Path)
p.add_argument('--candidate-config',action='store_true')
a=p.parse_args()
names=['func_800571DC','func_80059648','func_800597A0','func_80058600',
       'func_8001A250','func_8001295C','func_80080450','func_800826E8',
       'func_80082798','func_800823AC','func_800806B4','func_80080768',
       'func_800807C0','func_80080820','func_80080890','func_8005980C',
       'func_80083408','func_80083210','func_800817C0','func_80081914',
       'func_80081FEC','func_80082180','func_800826B0','n_alEnvmixerPull',
       'func_800129F8','func_8001291C','func_8004F348','func_800824E8']
# Execute the real initializer too: a larger logical row count alone does not
# prove that its heap, physical voices, DMA cache and command storage fit.
names += ['func_8007FE9C','MusSetMasterVolume','__MusIntMemMalloc',
          'func_80080D90','func_80084290','func_80086C70','__OsSchedInstall',
          'alCopy','alHeapDBAlloc','alHeapInit','alLink','alN_PVoiceNew','alUnlink',
          'func_800835F0','func_80083658','func_800838C8','func_80083918',
          'func_80083A60','func_80083D40','func_80083D58','func_80083D9C',
          'func_80083E40','func_80084084','func_80084440','func_80084700',
          'func_80084830','func_800848AC','func_800848C4','func_800848D0',
          'func_80086CC0','func_80086DD8','func_80087608','init_lpfilter',
          'n_alEnvmixerParam','n_alLoadParam']
functions={}
all_functions={}
for path in a.generated.glob('funcs_*.c'):
    for m in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)',path.read_text(),re.S):
        all_functions[m[1]]=m[0]
        if m[1] in names:functions[m[1]]=m[0]
assert len(functions)==len(names)
hooks=[]
for line in a.config.read_text().splitlines():
    if 'rr64_rival_engine_' not in line or line.lstrip().startswith('#'):continue
    m=re.fullmatch(r'\s*\{\s*func\s*=\s*"(\w+)"\s*,\s*(?:before_vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*)?text\s*=\s*(".*")\s*\},?\s*',line)
    assert m,('audit changed hook format',line)
    hook=dict(func=m[1],address=int(m[2],16) if m[2] else int(m[1][5:],16),text=json.loads(m[3]))
    hooks.append(hook)
    if hook['func'] not in functions:
        # Production mode also audits frame, mode, recovery and audio-init
        # integration even though the fixture invokes their helpers directly.
        if not a.candidate_config:
            marker=f"    // 0x{hook['address']:08X}:"
            raw=all_functions[hook['func']]
            assert raw.count(marker)==1 and raw.split(marker)[0].rstrip().endswith(hook['text']),('integration hook missing',hook)
        continue
    code=functions[hook['func']]
    marker=f"    // 0x{hook['address']:08X}:"
    assert code.count(marker)==1,hook
    if not code.split(marker)[0].rstrip().endswith(hook['text']):
        assert a.candidate_config,('production hook missing',hook)
        code=code.replace(marker,'    '+hook['text']+'\n'+marker)
    functions[hook['func']]=code
assert hooks,'Rival engine hooks missing'
# Exact libaudio channel coefficients; external frame/audio processing is not
# needed to establish which physical channel receives the larger gain.
code=functions.pop('n_alEnvmixerPull')
body=code.split('L_800850D0:',1)[1].split('L_8008511C:',1)[0]
functions['rival_native_pan_coefficients']='RECOMP_FUNC void rival_native_pan_coefficients(uint8_t* rdram,recomp_context* ctx) {\nuint64_t hi=0,lo=0,result=0;\n'+body+'\n}\n'
names[names.index('n_alEnvmixerPull')]='rival_native_pan_coefficients'
# Original traffic relative-position/velocity calculation, including its native
# projection, ROM coefficient and clamp. This independently verifies the rival
# Doppler adaptation without embedding a copy of its C++ formula in the test.
traffic=all_functions['func_80057B3C']
body=traffic.split('    // 0x80057D48:',1)[1].split('    // 0x80057DB8:',1)[0]
functions['rival_native_traffic_doppler']=(
    'RECOMP_FUNC void rival_native_traffic_doppler(uint8_t* rdram,recomp_context* ctx) {\n'
    'uint64_t hi=0,lo=0,result=0;int c1cs=0;\n'
    '    // 0x80057D48:'+body+'\n; // The extracted sequence ends on a branch label.\n}\n')
names.append('rival_native_traffic_doppler')
# Unhooked native producer proves the AI gate and exact bike-state pitch curve.
stock=[]
for name in ('func_800571DC','func_80059648','func_8005980C'):
    code=functions[name]
    for h in hooks:code=code.replace(h['text'],'')
    code=re.sub(r'\s+rr64_online_audio_(?:owner|listener)\(rdram, ctx\);','',code)
    for n in ('func_800571DC','func_80059648','func_8005980C'):
        code=re.sub(r'\b'+n+r'\b','stock_'+n,code)
    stock.append(code)
# Observe actual native entry points without replacing their allocation,
# update or stop bodies. The disabled-path regression proves these stay idle.
for name in ('func_800571DC','func_80080450','func_80082798','func_800806B4',
             'func_800807C0','func_80080820','func_80080890'):
    functions[name]=functions[name].replace('{',
        '{\n    rr64_rival_engine_fixture_call(0x'+name[-8:]+'u);',1)
calls=set(re.findall(r'^\s+(\w+)\(rdram, ctx\);',''.join(functions.values())+''.join(stock),re.M))
decls=('void rr64_rival_engine_fixture_call(unsigned);\n'
       'void (*rr64_rival_engine_fixture_lookup(unsigned))(unsigned char*,recomp_context*);\n'
       +'\n'.join('void '+n+'(unsigned char*,recomp_context*);' for n in sorted(calls|set(names)|{'stock_'+n for n in ('func_800571DC','func_80059648','func_8005980C')} ) if not n.startswith('rr64_')))
a.output.parent.mkdir(parents=True,exist_ok=True)
main=(a.config.parent.parent/'native/src/main.cpp').read_text()
queue=main.split('void queue_samples(int16_t* audio_data, size_t sample_count)',1)[1]
stereo=queue.split('    size_t i = 0;',1)[1].split('    rr64::achievement_audio::',1)[0]
stereo='void rival_host_stereo(int16_t* audio_data,size_t sample_count,int16_t* swapped) {\n    size_t i = 0;'+stereo+'}\n'
a.output.write_text('#include <cstddef>\n#include "recomp.h"\n#include "rr64_native.hpp"\n#include "rr64_rival_engine.hpp"\n#undef RECOMP_FUNC\n#define RECOMP_FUNC\n#undef LOOKUP_FUNC\n#define LOOKUP_FUNC(address) rr64_rival_engine_fixture_lookup(unsigned(address))\nextern "C" {\n'+decls+'\n'+''.join(functions[n] for n in names)+''.join(stock)+stereo+'}\n',encoding='utf-8')
print('Extracted',len(names),'native functions/sequences;',len(hooks),'rival hooks; candidate injection=',a.candidate_config)
print('External calls:',sorted(calls-set(names)-{'stock_func_800571DC','stock_func_80059648','stock_func_8005980C'}))
