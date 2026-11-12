$ErrorActionPreference = 'Stop'
$taskRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskConfigPath = Join-Path $taskRoot 'config/roadrash64.us.toml'
. (Join-Path $PSScriptRoot 'world_hook_contract.ps1')
Initialize-RR64WorldHookContract $taskRoot
$taskConfig = ConvertTo-RR64WorldFreeConfig ([System.IO.File]::ReadAllText($taskConfigPath))
$taskSourcePaths = @('funcs_2.c', 'funcs_7.c', 'funcs_14.c', 'funcs_15.c', 'funcs_17.c')
$taskFunctions = @{}
foreach ($taskFile in $taskSourcePaths) {
    $taskText = ConvertTo-RR64WorldFreeSource ([System.IO.File]::ReadAllText((Join-Path $taskRoot "build/RecompiledFuncs/$taskFile")))
    foreach ($taskMatch in [regex]::Matches($taskText,
            '(?s)RECOMP_FUNC void (?<name>\w+)\([^\n]+\) \{(?<body>.*?)(?=RECOMP_FUNC void|\z)')) {
        $taskFunctions[$taskMatch.Groups['name'].Value] = $taskMatch.Groups['body'].Value
    }
}

$taskHookCount = 0
foreach ($taskLine in ($taskConfig -split "`n")) {
    if ($taskLine -notmatch '^\s*\{ func = "(?<function>[^"]+)".*text = "(?<text>.*)" \},?\s*$' -or
        $taskLine -notmatch 'rr64_lod_') { continue }
    $taskDefinition = [regex]::Match($taskLine,
        '^\s*\{ func = "(?<function>[^"]+)".*text = "(?<text>.*)" \},?\s*$')
    $taskFunction = $taskDefinition.Groups['function'].Value
    $taskHook = $taskDefinition.Groups['text'].Value
    $taskBody = $taskFunctions[$taskFunction]
    if (-not $taskBody) { throw "Missing generated function: $taskFunction" }
    $taskAddress = [regex]::Match($taskLine, 'before_vram = (?<address>0x[0-9A-Fa-f]+)')
    if ($taskAddress.Success) {
        $taskPattern = [regex]::Escape($taskHook) + '\s*// ' +
            [regex]::Escape($taskAddress.Groups['address'].Value) + ':'
        if (-not [regex]::IsMatch($taskBody, $taskPattern)) {
            throw "Hook is missing or not immediately before the configured instruction: $taskFunction $taskHook"
        }
    }
    else {
        $taskEntry = ($taskBody -split '// 0x', 2)[0]
        foreach ($taskStatement in ($taskHook -split '(?<=;)\s+')) {
            if (-not $taskEntry.Contains($taskStatement)) {
                throw "Entry hook is not before the guest prologue: $taskFunction $taskStatement"
            }
        }
    }
    $taskHookCount++
}
if ($taskHookCount -ne 29) { throw "Expected 29 LOD hook definitions; found $taskHookCount" }

# Observe the original range result without changing its branch or delay slot.
# The renderer may reinterpret only its local hidden-bit value for the normal
# camera path, after the existing traffic visibility callback has completed.
if ($taskFunctions['func_8005EB50'] -notmatch 'c1cs = ctx->f4.fl < ctx->f2.fl;\s*// 0x8005EC24: nop\s*rr64_lod_observe_rider_range\(rdram, ctx->r17, c1cs\);\s*// 0x8005EC28: bc1f\s+L_8005ED2C\s*if \(!c1cs\)' -or
    $taskFunctions['func_80011CC0'] -notmatch 'ctx->r3 = ctx->r3 & 0X1;\s*ctx->r3 = rr64_traffic_render_visibility\(rdram, ctx->r19, ctx->r3\); if \(MEM_W\(0x50, ctx->r29\) == 0\) ctx->r3 = rr64_lod_actor_hidden\(rdram, ctx->r19, ctx->r3\);\s*// 0x80011D80: bne\s+\$v1, \$zero, L_800120C8\s*if \(ctx->r3 != 0\)') {
    throw 'Rider range provenance or guarded local hidden-bit override changed the original branch boundary.'
}

# Entry into AEE0's animation decision tree is not proof of fresh animation:
# B290 can still skip the actor. Certify actual calls only. B5EC is a shared
# skip destination, so its completion marker must consume the B5E4 token.
$taskAnimationMarkers = @{}
foreach ($taskMatch in [regex]::Matches($taskConfig,
        '\{ func = "func_8005AEE0", before_vram = (?<address>0x[0-9A-Fa-f]+), text = "rr64_lod_shadow_stage\(rdram, ctx->r19, (?<stage>\d+)\);" \}')) {
    $taskAnimationMarkers[$taskMatch.Groups['address'].Value] = $taskMatch.Groups['stage'].Value
}
$taskExpectedAnimationMarkers = @{
    '0x8005B3CC' = '4'; '0x8005B53C' = '4'; '0x8005B57C' = '4'
    '0x8005B5E4' = '8'; '0x8005B5EC' = '16'
}
if ($taskAnimationMarkers.Count -ne $taskExpectedAnimationMarkers.Count) {
    throw 'Unexpected animation freshness marker; entry/skip paths must not certify prepared poses.'
}
foreach ($taskAddress in $taskExpectedAnimationMarkers.Keys) {
    if ($taskAnimationMarkers[$taskAddress] -ne $taskExpectedAnimationMarkers[$taskAddress]) {
        throw "Missing completed-call marker or token boundary at $taskAddress"
    }
}
$taskAnimationBody = $taskFunctions['func_8005AEE0']
if ($taskAnimationBody -notmatch 'L_8005B480:\s*rr64_lod_shadow_full_weight\(rdram, ctx, 0\);\s*// 0x8005B480: jal\s+0x80018BD8' -or
    $taskAnimationBody -notmatch 'after_2:\s*rr64_lod_shadow_full_weight\(rdram, ctx, 1\);\s*// 0x8005B488: j\s+L_8005B5F0' -or
    $taskAnimationBody.Contains('L_8005B488:') -or
    [regex]::Matches($taskAnimationBody, 'rr64_lod_shadow_full_weight\(').Count -ne 2) {
    throw 'Full-weight animation must certify only the returned 18BD8 call, with the same rider before list advance.'
}
# 19130 applies an additive overlay. Its completion at B53C is only safe as
# freshness evidence because every route to that call already completed the
# base 18114 call at B3C4/B3CC. Check that control-flow property from the guest
# instruction comments; calls return after their delay slot, branches retain
# both possible outcomes, and the skipped delay slots contain no markers.
$taskAnimationInstructions = @{}
foreach ($taskMatch in [regex]::Matches($taskAnimationBody,
        '(?m)^\s*// 0x(?<address>[0-9A-Fa-f]{8}): (?<operation>\S+)(?<operands>[^\r\n]*)')) {
    $taskInstructionAddress = [Convert]::ToUInt32($taskMatch.Groups['address'].Value, 16)
    $taskAnimationInstructions[$taskInstructionAddress] = @{
        Operation = $taskMatch.Groups['operation'].Value
        Operands = $taskMatch.Groups['operands'].Value
    }
}
function Test-AnimationPath([uint32]$excludedAddress) {
    $taskPendingAddresses = [System.Collections.Generic.Stack[uint32]]::new()
    $taskVisitedAddresses = [System.Collections.Generic.HashSet[uint32]]::new()
    $taskPendingAddresses.Push([Convert]::ToUInt32('8005AEE0', 16))
    while ($taskPendingAddresses.Count) {
        $taskAddress = $taskPendingAddresses.Pop()
        if ($taskAddress -eq $excludedAddress -or -not $taskVisitedAddresses.Add($taskAddress)) { continue }
        if ($taskAddress -eq [Convert]::ToUInt32('8005B534', 16)) { return $true }
        $taskInstruction = $taskAnimationInstructions[$taskAddress]
        if (-not $taskInstruction) { throw ('Missing animation instruction: {0:X8}' -f $taskAddress) }
        $taskOperation = $taskInstruction.Operation
        if ($taskOperation -eq 'jr') { continue }
        if ($taskOperation -eq 'jal') {
            $taskPendingAddresses.Push($taskAddress + 8u)
        }
        elseif ($taskOperation -eq 'j' -or $taskOperation.StartsWith('b')) {
            $taskTarget = [regex]::Match($taskInstruction.Operands, 'L_(?<address>[0-9A-Fa-f]{8})')
            if (-not $taskTarget.Success) { throw "Unsupported animation branch: $taskOperation" }
            $taskPendingAddresses.Push([Convert]::ToUInt32($taskTarget.Groups['address'].Value, 16))
            if ($taskOperation -ne 'j') { $taskPendingAddresses.Push($taskAddress + 8u) }
        }
        else { $taskPendingAddresses.Push($taskAddress + 4u) }
    }
    return $false
}
if (-not (Test-AnimationPath 0) -or
    (Test-AnimationPath ([Convert]::ToUInt32('8005B3CC', 16)))) {
    throw 'The additive 19130 completion can no longer rely on a completed 18114 base pose.'
}
foreach ($taskAddress in @('0x8005B3CC', '0x8005B53C', '0x8005B57C')) {
    if ($taskAnimationBody.Contains(('L_' + $taskAddress.Substring(2) + ':'))) {
        throw "Animation completion acquired a branch entry that could bypass its helper: $taskAddress"
    }
}
if ($taskAnimationBody -match 'rr64_lod_shadow_stage\([^\n]+\);\s*// 0x8005B0A8:' -or
    $taskAnimationBody -notmatch 'L_8005B5E4:\s*rr64_lod_shadow_stage\(rdram, ctx->r19, 8\);\s*// 0x8005B5E4: jal\s+0x80018114' -or
    $taskAnimationBody -notmatch 'L_8005B5EC:\s*rr64_lod_shadow_stage\(rdram, ctx->r19, 16\);\s*// 0x8005B5EC: lw\s+\$s3') {
    throw 'The shared animation call/skip boundary is not guarded by matching pending/completion markers.'
}
foreach ($taskAddress in @('0x80011F00', '0x800120C8', '0x800120D4')) {
    if (-not [regex]::IsMatch($taskFunctions['func_80011CC0'],
            'rr64_lod_end_actor\(\);\s*// ' + [regex]::Escape($taskAddress) + ':')) {
        throw "Missing pose restoration at actor completion, skip or function exit: $taskAddress"
    }
}

# BE98/BE9C are shared bike skip targets, including a missing second wheel
# after the first has been updated. The marker precedes the final unbranched
# store at BE94, and publication occurs only after the complete pass returns.
$taskBikeBody = $taskFunctions['func_8005B948']
if ($taskBikeBody -notmatch 'rr64_lod_shadow_stage\(rdram, ctx->r23, 128\);\s*// 0x8005BE94: swc1[^\r\n]*\r?\n\s*MEM_W\(0X0, ctx->r2\) = ctx->f2.u32l;\s*L_8005BE98:' -or
    [regex]::Matches($taskBikeBody, 'rr64_lod_shadow_stage\(').Count -ne 1) {
    throw 'Bike pose freshness must be marked at the final store before all shared skip targets.'
}

# Apart from injected hooks, every existing generated instruction and native
# call must survive exactly. This reads source files only; no ROM is opened.
foreach ($taskFile in @('funcs_2.c', 'funcs_14.c', 'funcs_15.c', 'funcs_17.c')) {
    $taskOldPath = Join-Path $taskRoot "handoff/stable-presentation-r1-before/build/RecompiledFuncs/$taskFile"
    $taskOld = [System.IO.File]::ReadAllText($taskOldPath) -replace '\r\n', "`n"
    $taskNew = ConvertTo-RR64WorldFreeSource ([System.IO.File]::ReadAllText((Join-Path $taskRoot "build/RecompiledFuncs/$taskFile")))
    # Keep the existing engine callback when the generator puts both entry
    # hooks on one line, then remove only the LOD statements under review.
    $taskNew = $taskNew.Replace('rr64_lod_invalidate(rdram); rr64_engine_capture_race_update_boundary(rdram, 0);', 'rr64_engine_capture_race_update_boundary(rdram, 0);')
    $taskNew = $taskNew.Replace('ctx->r3 = rr64_traffic_render_visibility(rdram, ctx->r19, ctx->r3); if (MEM_W(0x50, ctx->r29) == 0) ctx->r3 = rr64_lod_actor_hidden(rdram, ctx->r19, ctx->r3);', 'ctx->r3 = rr64_traffic_render_visibility(rdram, ctx->r19, ctx->r3);')
    $taskNew = $taskNew -replace '(?m)^    (?:rr64_lod_[^\n]*|ctx->r(?:21|15) = rr64_lod_[^\n]*|if \(rr64_lod_[^\n]*|if \(MEM_W\(0x50, ctx->r29\) == 0\) ctx->r21 = rr64_lod_select\(rdram, ctx->r19, ctx->r21\);)\n', ''
    if ($taskOld -cne $taskNew) { throw "Unexpected generated source change beyond LOD hooks: $taskFile" }
}

$taskPrompt = 'rr64_write_button_prompt(rdram, RR64_PROMPT_BUTTON_B, 0x80003B6C, 2); rr64_write_button_prompt(rdram, RR64_PROMPT_BUTTON_A, 0x800025F8, 2);'
if (-not $taskConfig.Contains($taskPrompt) -or
    -not $taskFunctions['func_8002F364'].Contains($taskPrompt)) {
    throw 'The original A/B prompt hook is missing or changed.'
}
$taskDraw = $taskFunctions['func_8006A638']
# Only a draw that rebuilt its actor inputs may prepare a replacement. The
# cached branch must continue directly to AB18 and retain the update snapshot.
if ($taskDraw -notmatch 'after_39:\s*rr64_lod_prepare_shadow\(rdram, ctx, 0\);\s*// 0x8006AB14: lui[^\r\n]*\r?\n\s*ctx->r3 = S32\(0X800A << 16\);\s*L_8006AB18:' -or
    $taskDraw -notmatch '// 0x8006AAD4: bne[^\r\n]*L_8006AB18' -or
    [regex]::Matches($taskDraw, 'rr64_lod_prepare_shadow\(rdram, ctx, 0\);').Count -ne 1) {
    throw 'The uncached single-view preparation must run before AB14, outside the cached AB18 branch.'
}
if ([regex]::Matches($taskDraw, 'rr64_lod_begin_draw\(').Count -ne 1 -or
    [regex]::Matches($taskDraw, 'rr64_lod_end_draw\(').Count -ne 1 -or
    [regex]::Matches($taskDraw, '(?m)^    return;').Count -ne 1 -or
    [regex]::Matches($taskDraw, 'goto L_8006AF(?:C[0-9A-F]|D[0-9A-F]|E[0-9A-F]|F[0-9A-F])').Count -ne 0) {
    throw 'The race draw scope does not cover its complete return path.'
}
Write-Output "LOD hook source verification passed: $taskHookCount retained actor placements plus 24 exact world definitions, base animation dominance, guarded animation/bike skip boundaries, all 28 generated C files preserved, complete actor/draw scope, original A/B prompt retained."
