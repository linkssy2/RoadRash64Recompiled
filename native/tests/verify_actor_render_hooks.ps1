param([string]$Repository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'world_hook_contract.ps1')
Initialize-RR64WorldHookContract $Repository
$config = ConvertTo-RR64WorldFreeConfig (Get-Content -Raw -LiteralPath (Join-Path $Repository 'config/roadrash64.us.toml'))
$checks = @(
    @('funcs_2.c','80011B08','rr64_lod_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19);'),
    @('funcs_2.c','80011C14','rr64_lod_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19);'),
    @('funcs_2.c','80011D80','ctx->r3 = rr64_traffic_render_visibility(rdram, ctx->r19, ctx->r3); if (MEM_W(0x50, ctx->r29) == 0) ctx->r3 = rr64_lod_actor_hidden(rdram, ctx->r19, ctx->r3);'),
    @('funcs_2.c','80011D8C','if (MEM_W(0x50, ctx->r29) == 0) ctx->r21 = rr64_lod_select(rdram, ctx->r19, ctx->r21);'),
    @('funcs_2.c','80011DDC','ctx->r15 = rr64_lod_root_source(rdram, ctx->r19, ctx->r18, ctx->r15);'),
    @('funcs_2.c','80011E80','rr64_lod_scale_root_matrix(rdram, ctx->r19, ctx->r18, ctx->r29 + 0x10);'),
    @('funcs_2.c','80011F00','rr64_lod_end_actor();'),
    @('funcs_2.c','800120C8','rr64_lod_end_actor();'),
    @('funcs_2.c','800120D4','rr64_lod_end_actor();'),
    @('funcs_14.c','8005E6D8','rr64_lod_observe_pair(rdram, ctx);'),
    @('funcs_14.c','8005B030','if (rr64_lod_shadow_rider(rdram, ctx->r19)) c1cs = 0;'),
    @('funcs_14.c','8005B3CC','rr64_lod_shadow_stage(rdram, ctx->r19, 4);'),
    @('funcs_14.c','8005B480','rr64_lod_shadow_full_weight(rdram, ctx, 0);'),
    @('funcs_14.c','8005B488','rr64_lod_shadow_full_weight(rdram, ctx, 1);'),
    @('funcs_14.c','8005B53C','rr64_lod_shadow_stage(rdram, ctx->r19, 4);'),
    @('funcs_14.c','8005B57C','rr64_lod_shadow_stage(rdram, ctx->r19, 4);'),
    @('funcs_14.c','8005B5E4','rr64_lod_shadow_stage(rdram, ctx->r19, 8);'),
    @('funcs_14.c','8005B5EC','rr64_lod_shadow_stage(rdram, ctx->r19, 16);'),
    @('funcs_14.c','8005BE94','rr64_lod_shadow_stage(rdram, ctx->r23, 128);'),
    @('funcs_15.c','8005EA94','rr64_lod_shadow_stage(rdram, ctx->r17, 1);'),
    @('funcs_15.c','8005EC64','rr64_lod_shadow_stage(rdram, ctx->r17, 2);'),
    @('funcs_15.c','8005EC28','rr64_lod_observe_rider_range(rdram, ctx->r17, c1cs);'),
    @('funcs_17.c','8006A714','rr64_lod_prepare_shadow(rdram, ctx, 1);'),
    @('funcs_17.c','8006AB14','rr64_lod_prepare_shadow(rdram, ctx, 0);'),
    @('funcs_17.c','8006B6DC','rr64_lod_prepare_shadow(rdram, ctx, 0);'),
    @('funcs_17.c','8006AFBC','rr64_lod_end_draw(rdram);')
)
foreach ($check in $checks) {
    $source = ConvertTo-RR64WorldFreeSource (Get-Content -Raw -LiteralPath (Join-Path $Repository ('build/RecompiledFuncs/' + $check[0])))
    $pattern = [regex]::Escape($check[2]) + '\s*// 0x' + $check[1] + ':'
    if ([regex]::Matches($source, $pattern).Count -ne 1) {
        throw ('Generated hook missing or duplicated at ' + $check[1])
    }
    if ($config -notmatch ('before_vram = 0x' + $check[1] + ', text = "' + [regex]::Escape($check[2]) + '"')) {
        throw ('Configuration hook mismatch at ' + $check[1])
    }
}
foreach ($entry in @(
    @('funcs_14.c','func_8005D9A4','rr64_lod_begin_preparation(rdram);'),
    @('funcs_17.c','func_8006A638','rr64_lod_begin_draw(rdram);'),
    @('funcs_17.c','func_8006AFFC','rr64_lod_invalidate(rdram);')
)) {
    $source = ConvertTo-RR64WorldFreeSource (Get-Content -Raw -LiteralPath (Join-Path $Repository ('build/RecompiledFuncs/' + $entry[0])))
    # N64Recomp emits the entry hook after its two local declarations. Older
    # hand-inserted output placed it immediately after the opening brace.
    $locals = '(?:uint64_t hi = 0, lo = 0, result = 0;\s*int c1cs = 0;\s*)?'
    if ($source -notmatch ('void ' + $entry[1] + '\([^\n]*\) \{\s*' + $locals + [regex]::Escape($entry[2]))) {
        throw ('Missing entry scope for ' + $entry[1])
    }
    if ($config -notmatch ('func = "' + $entry[1] + '", text = "' + [regex]::Escape($entry[2]))) {
        throw ('Missing configured entry scope for ' + $entry[1])
    }
}
if ($config -match 'rr64_actor_begin_presentation|rr64_actor_select_render_lod|rr64_actor_restore_presentation') {
    throw 'Legacy actor transaction unexpectedly reactivated'
}
if ($config -match 'before_vram = 0x8005B0A8, text = "rr64_lod_shadow_stage') {
    throw 'Animation entry must not certify completed pose preparation'
}
Write-Output '[RR64-LOD-HOOKS] PASS: 29 retained actor hooks and 24 exact world hook definitions; all 28 original generated C files preserved; legacy transactions disconnected'
