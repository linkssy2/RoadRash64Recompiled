param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs')
)
$ErrorActionPreference = 'Stop'
$names = @('func_80018668', 'func_8005ED6C', 'func_8005EF74', 'func_8005F070',
    'func_80015A90', 'func_800157C4', 'func_8001348C', 'func_80013284',
    'func_80012D58', 'func_80012DBC', 'func_8001A634', 'func_80013468',
    'func_80012CE0', 'func_80012BE8', 'func_80015634', 'func_8009C9F0',
    '__sinf_recomp', '_nsqrtf', 'func_8009C860', 'func_800167BC', 'guPerspective',
    'guLookAt', 'guMtxIdentF', 'guMtxF2L', '__cosf_recomp')
$hooks = @('rr64_world_camera_far', 'rr64_world_camera_normalization')
$found = @{}
foreach ($file in Get-ChildItem -LiteralPath $GeneratedDirectory -Filter '*.c' | Sort-Object Name) {
    $source = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($name in $names) {
        $match = [regex]::Match($source,
            '(?ms)^RECOMP_FUNC void ' + [regex]::Escape($name) + '\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)')
        if ($match.Success) {
            if ($found.ContainsKey($name)) { throw "Duplicate generated function: $name" }
            $found[$name] = $match.Value
        }
    }
}
$result = "// Exact original world root, pedestrian animation and transitive math oracle; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"rr64_world_render.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    foreach ($call in [regex]::Matches($found[$name], '(?m)^\s+([A-Za-z0-9_]+)\(rdram, ctx\);')) {
        if ($call.Groups[1].Value -notin $names -and $call.Groups[1].Value -notin $hooks) { throw "Uncovered transitive call: $name -> $($call.Groups[1].Value)" }
    }
    $result += $found[$name] + "`n"
}
$output = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output)) | Out-Null
if (-not [System.IO.File]::Exists($output) -or [System.IO.File]::ReadAllText($output) -cne $result) {
    [System.IO.File]::WriteAllText($output, $result, [System.Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged world animation/root/math functions."
