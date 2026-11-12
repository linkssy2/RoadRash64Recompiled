# R18 exact world integration contract; each definition is reviewed and frozen.
$RR64ExpectedWorldDefinitions = @(
    '{ func = "func_8006AFFC", text = "rr64_lod_invalidate(rdram); rr64_world_invalidate(rdram); rr64_engine_capture_race_update_boundary(rdram, 0);" },'
    '{ func = "func_80011988", before_vram = 0x80011B08, text = "rr64_lod_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19); rr64_world_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19);" },'
    '{ func = "func_80011988", before_vram = 0x80011C14, text = "rr64_lod_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19); rr64_world_observe_allocation(rdram, ctx->r20, ctx->r16, ctx->r19);" },'
    '{ func = "func_8005D9A4", text = "rr64_lod_begin_preparation(rdram); rr64_world_invalidate(rdram);" },'
    '{ func = "func_8006A638", text = "rr64_lod_begin_draw(rdram); rr64_world_begin_draw(rdram);" },'
    '{ func = "func_8006A638", before_vram = 0x8006AFBC, text = "rr64_lod_end_draw(rdram); rr64_world_end_draw(rdram);" },'
    '{ func = "func_80011CC0", before_vram = 0x80011D8C, text = "if (MEM_W(0x50, ctx->r29) == 0) { ctx->r21 = rr64_lod_select(rdram, ctx->r19, ctx->r21); ctx->r21 = rr64_world_select(rdram, ctx->r19, ctx->r21); }" },'
    '{ func = "func_80011CC0", before_vram = 0x80011DDC, text = "ctx->r15 = rr64_lod_root_source(rdram, ctx->r19, ctx->r18, ctx->r15); ctx->r15 = rr64_world_root_source(rdram, ctx->r19, ctx->r18, ctx->r15);" },'
    '{ func = "func_80011CC0", before_vram = 0x80011E80, text = "rr64_lod_scale_root_matrix(rdram, ctx->r19, ctx->r18, ctx->r29 + 0x10); rr64_world_scale_root_matrix(rdram, ctx->r19, ctx->r18, ctx->r29 + 0x10);" },'
    '{ func = "func_80011CC0", before_vram = 0x80011F00, text = "rr64_lod_end_actor(); rr64_world_end_actor();" },'
    '{ func = "func_80011CC0", before_vram = 0x800120C8, text = "rr64_lod_end_actor(); rr64_world_end_actor();" },'
    '{ func = "func_80011CC0", before_vram = 0x800120D4, text = "rr64_lod_end_actor(); rr64_world_end_actor();" },'
    '{ func = "func_8005ED6C", before_vram = 0x8005EF48, text = "rr64_world_observe_roots(rdram, 3);" },'
    '{ func = "func_8005EF74", before_vram = 0x8005F050, text = "rr64_world_observe_roots(rdram, 4);" },'
    '{ func = "func_8005F070", before_vram = 0x8005F13C, text = "rr64_world_observe_roots(rdram, 5);" },'
    '{ func = "func_800167BC", before_vram = 0x80016888, text = "rr64_world_camera_far(rdram, ctx);" },'
    '{ func = "func_800167BC", before_vram = 0x80016910, text = "rr64_world_camera_normalization(rdram, ctx);" },'
    '{ func = "func_8007C524", text = "rr64_world_terrain_begin(rdram); rr64_world_objects_begin(rdram);" },'
    '{ func = "func_8007C524", before_vram = 0x8007C708, text = "rr64_world_terrain_observe(rdram, MEM_W(0, ctx->r16));" },'
    '{ func = "func_8007C524", before_vram = 0x8007C748, text = "rr64_world_terrain_draw(rdram);" },'
    '{ func = "func_8007CDA4", before_vram = 0x8007CF3C, text = "rr64_world_objects_observe(rdram, ctx->r17);" },'
    '{ func = "func_8007CDA4", before_vram = 0x8007CF44, text = "rr64_world_objects_sample(rdram, ctx->r17, ctx->r19);" },'
    '{ func = "func_8007C524", before_vram = 0x8007C7D0, text = "rr64_world_objects_draw(rdram);" },'
    '{ func = "func_80011CC0", before_vram = 0x80011D80, text = "if (rr64_world_distance_enabled()) { if (MEM_W(0x50, ctx->r29) == 0) ctx->r3 = rr64_world_actor_hidden(rdram, ctx->r19, ctx->r3, ctx); } else ctx->r3 = rr64_traffic_render_visibility(rdram, ctx->r19, ctx->r3); if (MEM_W(0x50, ctx->r29) == 0) ctx->r3 = rr64_lod_actor_hidden(rdram, ctx->r19, ctx->r3);" },'
)
function Get-RR64HookDefinitions([string]$Text) {
    $definitions = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*\{ func = "(?<function>[^"]+)"(?:, before_vram = (?<address>0x[0-9A-Fa-f]+))?, text = "(?<text>.*)" \},?\s*$')
        if (-not $match.Success) { continue }
        $key = $match.Groups['function'].Value + ':' + $match.Groups['address'].Value
        if ($definitions.ContainsKey($key)) { throw "Duplicate configured hook: $key" }
        $definitions[$key] = [pscustomobject]@{function=$match.Groups['function'].Value;address=$match.Groups['address'].Value;text=$match.Groups['text'].Value;line=$line.Trim()}
    }
    return $definitions
}
function Initialize-RR64WorldHookContract([string]$Repository) {
    $script:RR64WorldCurrentConfig = [IO.File]::ReadAllText((Join-Path $Repository 'config/roadrash64.us.toml'))
    $actualLines = @($script:RR64WorldCurrentConfig -split "`r?`n" | Where-Object { $_ -match 'rr64_world_' } | ForEach-Object { $_.Trim() })
    if ($actualLines.Count -ne 24 -or (Compare-Object ($RR64ExpectedWorldDefinitions | Sort-Object) ($actualLines | Sort-Object))) {
        throw 'World hook contract differs from the 24 reviewed exact definitions.'
    }
    $script:RR64WorldCurrent = Get-RR64HookDefinitions $script:RR64WorldCurrentConfig
    $script:RR64WorldBaseline = Get-RR64HookDefinitions ([IO.File]::ReadAllText((Join-Path $Repository 'handoff/presentation-r18-world-distance-before/config/roadrash64.us.toml')))
    $script:RR64WorldKeys = @($script:RR64WorldCurrent.Keys | Where-Object {$script:RR64WorldCurrent[$_].text -match 'rr64_world_'})
    $functions = @{}
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Repository 'build/RecompiledFuncs') -Filter '*.c') {
        $source = [IO.File]::ReadAllText($file.FullName)
        foreach ($match in [regex]::Matches($source, '(?s)RECOMP_FUNC void (?<name>\w+)\([^\n]+\) \{(?<body>.*?)(?=RECOMP_FUNC void|\z)')) {
            $functions[$match.Groups['name'].Value] = $match.Groups['body'].Value
        }
    }
    foreach ($key in $script:RR64WorldKeys) {
        $hook = $script:RR64WorldCurrent[$key]; $body = $functions[$hook.function]
        if (-not $body) { throw "World hook function missing: $key" }
        if ($hook.address) {
            $pattern = [regex]::Escape($hook.text) + '\s*// ' + [regex]::Escape($hook.address) + ':'
        } else {
            $pattern = '^\s*(?:uint64_t hi = 0, lo = 0, result = 0;\s*int c1cs = 0;\s*)?' + [regex]::Escape($hook.text)
        }
        if ([regex]::Matches($body, $pattern).Count -ne 1) { throw "Missing/duplicated world hook instruction or entry placement: $key" }
    }
    # Preserve every guest operation and prior hook in all 28 generated C files.
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Repository 'handoff/presentation-r18-world-distance-before/build/RecompiledFuncs') -Filter '*.c') {
        $old = [IO.File]::ReadAllText($file.FullName) -replace '\r\n', "`n"
        $current = [IO.File]::ReadAllText((Join-Path $Repository ('build/RecompiledFuncs/' + $file.Name)))
        if ($old -cne (ConvertTo-RR64WorldFreeSource $current)) { throw "Generated guest or prior-hook drift beyond reviewed world hooks: $($file.Name)" }
    }
}
function ConvertTo-RR64WorldFreeSource([string]$Text) {
    $result = $Text -replace '\r\n', "`n"
    foreach ($key in $script:RR64WorldKeys) {
        $hook = $script:RR64WorldCurrent[$key]
        if ($script:RR64WorldBaseline.ContainsKey($key)) {
            $result = $result.Replace($hook.text, $script:RR64WorldBaseline[$key].text)
        } else {
            $result = $result -replace ('(?m)^    ' + [regex]::Escape($hook.text) + '\n'), ''
        }
    }
    return $result
}
function ConvertTo-RR64WorldFreeConfig([string]$Text) {
    $result = $Text
    foreach ($key in $script:RR64WorldKeys) {
        $hook = $script:RR64WorldCurrent[$key]
        if ($script:RR64WorldBaseline.ContainsKey($key)) {
            $result = $result.Replace($hook.text, $script:RR64WorldBaseline[$key].text)
        } else {
            $result = $result -replace ('(?m)^\s*' + [regex]::Escape($hook.line) + '\r?\n'), ''
        }
    }
    return $result
}
