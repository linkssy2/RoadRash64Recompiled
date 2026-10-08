"""Generate a closed private native update. No ROM bytes or game assets.

Only direct calls and audited actor/collision callback targets are admitted.
Unknown indirect targets fail the disposable transaction instead of calling
the live runtime. The original generated functions remain unchanged.
"""
from pathlib import Path
import re
import sys

source,output=map(Path,sys.argv[1:])
functions,addresses={},{}
for path in sorted(source.glob('funcs_*.c')):
    for match in re.finditer(r'RECOMP_FUNC void (\w+)\(.*?(?=RECOMP_FUNC void|\Z)',path.read_text(),re.S):
        name,body=match[1],match[0]
        functions[name]=body
        address=re.search(r'// 0x([0-9A-F]{8}):',body)
        if address:addresses[address[1]]=name
callbacks='''800524CC 8005264C 80040500 80040558 800405E4 8004090C
8004EB6C 8004EAD0 800515C4 8005F570 8005F7A4 8005FA18 8005FDF0 8005FF10
80060008 80060188 80060370 8006093C 80060A84 80060B04 80060B28 80060B4C
80060B70 80060B94 80060BB8 80099448
8006BEE4 8006BFF4 8006C120'''.split()
# V18 observed race-order callbacks for cop activation and roadside traffic
# creation. Their native dependency closures stay private and use the existing
# resource queue services. Keep unknown targets rejected; do not fall back to
# live function lookup when a track dispatches one of these events.
# sprintf passes proutSprintf (99448) to _Printf indirectly. It only copies
# formatted bytes into the supplied private guest buffer and returns its end;
# no host output or live runtime service. Direct-call closure cannot find it.
body=functions['func_8006AFFC']
prefix='void native_frame(uint8_t* rdram,recomp_context* ctx){\nuint64_t hi=0,lo=0,result=0;int c1cs=0;\n'
prefix+='    // 0x8006AFFC:'+body.split('    // 0x8006AFFC:',1)[1].split('\nL_8006B678:',1)[0]
prefix+='\nL_8006B678:\nreturn;\n}\n'
calls=lambda text:set(re.findall(r'\b(\w+)\(rdram\s*,',text))
# Race ordering also advances recovery callbacks, eligibility, AI messages and
# random decisions. A sorting-only prefix omits those native state changes.
# Retain the full original routine in private memory. 6A254 and 6A380 are
# road-target steering calculations, not race transitions: they interpolate
# route points, derive an angle from rider position, and write the per-actor
# steering value at 800A6480. Their direct dependencies are included below.
order=functions['func_8006E5E0'].replace('RECOMP_FUNC void func_8006E5E0(', 'void native_order(',1)
todo=list(((calls(prefix)|calls(order))&functions.keys())|{addresses[a] for a in callbacks}|{'func_8003F0E8','func_8007B8D4'})
seen=set()
while todo:
    name=todo.pop()
    if name in seen:continue
    seen.add(name);todo.extend(calls(functions[name])&functions.keys())
external=set().union(*(calls(functions[n])-functions.keys() for n in seen))
allowed_os={'osRecvMesg_recomp','osSendMesg_recomp','osSetIntMask_recomp'}
if any(n.startswith('os') and n not in allowed_os for n in external):
    raise RuntimeError('New OS dependency requires explicit replay service: '+str(external))
parts=['#include "rr64_prediction_frame_begin.inc"']
parts.extend('void '+n+'(uint8_t*,recomp_context*);' for n in sorted(seen))
parts.append('using NativeFunction=void(*)(uint8_t*,recomp_context*);\nNativeFunction private_lookup(gpr address){switch(static_cast<uint32_t>(address)){')
parts.extend('case 0x'+a+':return '+n+';' for a,n in sorted(addresses.items()) if n in seen)
parts.append('default:{char reason[96];std::snprintf(reason,sizeof(reason),"unregistered native replay callback %08x",static_cast<unsigned>(address));throw Resources::Invalid(reason);}}}')
for text in [*(functions[n] for n in sorted(seen)),prefix,order]:
    # Terrain cache readiness is captured with each historical streaming
    # event. Never initialize/query today's renderer from a private image.
    text = text.replace('rr64_terrain_streaming_range(rdram, ctx->f4.u32l)',
                        'private_terrain_streaming_range(ctx->f4.u32l)')
    text = text.replace('rr64_terrain_pool_require(rdram, 2u, (unsigned)ctx->r16)',
                        'private_terrain_pool_require(rdram, 2u, (unsigned)ctx->r16)')
    if 'rr64_terrain_streaming_range(' in text or 'rr64_terrain_pool_require(' in text:
        raise RuntimeError('New terrain pool hook requires explicit private replay handling')
    # These register-only presentation hooks consult the live peer's listener.
    # Historical simulation keeps the original native gain path and must never
    # read today's network session or cause a live audio service call.
    text=re.sub(r'\brr64_online_audio_(?:owner|listener|weapon_gain)\(rdram, ctx\);',
                '(void)0; // Live listener hook omitted from private replay.',text)
    text=re.sub(r'\brr64_online_audio_weapon_source\(rdram, ctx, \(unsigned\)ctx->r(?:4|16)\);',
                '(void)0; // Live listener hook omitted from private replay.',text)
    if 'rr64_online_audio_' in text:
        raise RuntimeError('New online audio hook requires explicit private replay handling')
    # Weapon cycling remains native physics/input, but its HUD focus belongs
    # to the visible frame, never to an older private correction frame.
    text=re.sub(r'\brr64_course_items_weapon_(?:switched|wrapped)\(rdram,\(unsigned\)ctx->r(?:4|16)\);',
                '(void)0; // Live inventory HUD focus omitted from private replay.',text)
    if 'rr64_course_items_weapon_switched' in text or 'rr64_course_items_weapon_wrapped' in text:
        raise RuntimeError('New weapon HUD hook requires explicit private replay handling')
    # Rival engines and their allocation budget are live presentation only.
    # The original private sound path still uses its isolated mixer services.
    text=re.sub(r'if \(rr64_rival_engine_(?:gain|allocate)\(rdram, ctx\)\) return;',
                '(void)0; // Live rival engine hook omitted from private replay.',text)
    text=re.sub(r'\brr64_rival_engine_(?:frame|gate|threshold|pitch|transition|no_steal|allocated|child_adopt|heap|audio_init)\(rdram, ctx\);',
                '(void)0; // Live rival engine hook omitted from private replay.',text)
    text=re.sub(r'\brr64_rival_engine_(?:mode|recovery)\(rdram, ctx, \(unsigned\)ctx->r(?:4|17)\);',
                '(void)0; // Live rival engine hook omitted from private replay.',text)
    text=re.sub(r'\brr64_rival_engine_(?:child|allocation_range)\(rdram, ctx, [01]\);|\brr64_rival_engine_audio_reset\(\);',
                '(void)0; // Live rival engine hook omitted from private replay.',text)
    text=re.sub(r'ctx->r([237]) = rr64_rival_engine_effect_limit\(\(unsigned\)ctx->r\1\);',
                '(void)0; // Live sound-pool partition omitted from private replay.',text)
    if 'rr64_rival_engine_' in text:
        raise RuntimeError('New rival engine hook requires explicit private replay handling')
    # Inventory, projectiles, cues and display are live-only. Physics effects
    # are explicitly seeded by frame_end from historical state. The entry
    # before_physics call is supplied there once (the prefix drops entry hooks),
    # while native immunity/ghost queries remain in their audited consumers.
    text=re.sub(r'\brr64_mk64_items_(?:step|mode|scale_matrix|draw|hud|bike_contact)\([^;]*\);',
                '(void)0; // Live MK64 item simulation/presentation omitted from private replay.',text)
    item_hooks=set(re.findall(r'\b(rr64_mk64_items_\w+)\(',text))
    if item_hooks-{'rr64_mk64_items_before_physics','rr64_mk64_items_immune','rr64_mk64_items_ghost'}:
        raise RuntimeError('New MK64 item hook requires explicit private replay handling: '+str(item_hooks))
    # Verifier-only cell dependency probe at the original, already-computed
    # indices. Do not duplicate float-to-cell conversion or touch live code.
    if text.startswith('RECOMP_FUNC void func_800146C8('):
        marker='    // 0x80014760:'
        if text.count(marker)!=1:raise RuntimeError('Terrain query probe boundary changed')
        text=text.replace(marker,'    probe_terrain_query(rdram,static_cast<unsigned>(ctx->r9),static_cast<unsigned>(ctx->r6));\n'+marker)
        load='    ctx->r5 = MEM_BU(ctx->r4, 0XC);'
        if text.count(load)!=1:raise RuntimeError('Terrain availability load changed')
        text=text.replace(load,load+'\n    ctx->r5 = historical_terrain_state(static_cast<unsigned>(ctx->r4),static_cast<unsigned>(ctx->r5));')
    if text.startswith('RECOMP_FUNC void func_80041090('):
        marker='    // 0x80041090:'
        if text.count(marker)!=1:raise RuntimeError('Bike height probe boundary changed')
        text=text.replace(marker,'    TerrainBikeScope terrain_bike_scope(static_cast<unsigned>(ctx->r4));\n'+marker)
    if text.startswith('RECOMP_FUNC void func_800784B4('):
        marker='    ctx->r2 = ctx->r2 & 0X1;'
        if text.count(marker)!=1:raise RuntimeError('Visibility predicate boundary changed')
        text=text.replace(marker,'    ctx->r2 = historical_visibility_bit(static_cast<unsigned>(ctx->r6),static_cast<unsigned>(ctx->r2));')
        for old,new in [
          ('ctx->r4 = MEM_W(ctx->r2, 0X4F24);','ctx->r4 = historical_visibility_parameter(true,MEM_W(ctx->r2, 0X4F24));'),
          ('ctx->f2.u32l = MEM_W(ctx->r2, -0X239C);','ctx->f2.u32l = historical_visibility_parameter(false,MEM_W(ctx->r2, -0X239C));'),
          ('ctx->f0.u32l = MEM_W(ctx->r4, 0X0);','ctx->f0.u32l = historical_visibility_distance(static_cast<unsigned>(ctx->r6),static_cast<unsigned>(ctx->r4),MEM_W(ctx->r4, 0X0));')]:
            if text.count(old)!=1:raise RuntimeError('Visibility distance load changed')
            text=text.replace(old,new)

    # GCC rejects gotos across initialized scalar declarations in generated C++.
    # Hoist only declarations, retaining assignments at their native instruction.
    # Keep the accepted Clang/Windows preprocessing path unchanged.
    jump_temporaries = re.findall(r'    gpr (jr_addend_[0-9A-F]+) = ([^;]+);', text)
    if jump_temporaries:
        declarations = "\n#if defined(__GNUC__) && !defined(__clang__)\n"
        declarations += ''.join('    gpr ' + name + ';\n' for name, _ in jump_temporaries)
        declarations += '#endif\n'
        opening = text.index('{') + 1
        text = text[:opening] + declarations + text[opening:]
        for name, expression in jump_temporaries:
            original = '    gpr ' + name + ' = ' + expression + ';'
            text = text.replace(original,
                '#if defined(__GNUC__) && !defined(__clang__)\n    ' + name + ' = ' + expression + ';\n'
                '#else\n' + original + '\n#endif')

    text=text.replace('RECOMP_FUNC void','void').replace('LOOKUP_FUNC(','private_lookup(')
    # Annotate ordinary generated scalar loads, retaining the original load
    # expression and instruction address. Stores have reversed argument order.
    # Disabled outside verification; this does not instrument the live game.
    annotated=[];pc=None
    sizes={'MEM_B':1,'MEM_BU':1,'MEM_H':2,'MEM_HU':2,'MEM_W':4,'LD':8}
    for line in text.splitlines(keepends=True):
        instruction=re.search(r'// 0x([0-9A-F]{8}):',line)
        if instruction:pc=instruction[1]
        if not line.lstrip().startswith('//') and pc:
            line=re.sub(r'\b(MEM_BU|MEM_B|MEM_HU|MEM_H|MEM_W|LD)\((ctx->r\d+), (-?0X[0-9A-F]+)\)',
                lambda m:f'(probe_dependency_read(rdram,static_cast<unsigned>({m[2]} + ({m[3]})),{sizes[m[1]]},0x{pc}),{m[0]})',line)
        annotated.append(line)
    text=''.join(annotated)
    # Error paths must discard this private transaction, not terminate the game.
    text=re.sub(r'\bdo_break\(', 'private_break(',text)
    text=re.sub(r'\bswitch_error\(', 'private_switch_error(',text)
    # Bound loops in malformed historical state. This budget exists only in
    # the disposable evaluator, not the original game simulation.
    text=re.sub(r'(^L_[0-9A-F]+:\s*\n)',r'\1private_step();\n',text,flags=re.M)
    text=re.sub(r'(void \w+\(uint8_t\* rdram,\s*recomp_context\* ctx\)\s*\{)',r'\1\nprivate_step();',text)
    parts.append(text)
parts.append('#include "rr64_prediction_frame_end.inc"')
output.parent.mkdir(parents=True,exist_ok=True)
output.write_text('\n'.join(parts))
print(f'Generated private update with {len(seen)} native dependencies')
