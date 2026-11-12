param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs')
)
$ErrorActionPreference = 'Stop'
$names = @('func_8005AEE0', 'func_80018BD8', 'func_80018114', 'func_80015634', 'func_800157C4',
    'func_8001348C', 'func_80013284', 'func_80012D58', 'func_80013228',
    'func_8001A634', 'func_8009C9F0', 'func_8009C860', '__sinf_recomp', '_nsqrtf')
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
$result = "// Generated test oracle: exact animation and transitive math; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    foreach ($call in [regex]::Matches($found[$name], '(?m)^\s+([A-Za-z0-9_]+)\(rdram, ctx\);')) {
        if ($call.Groups[1].Value -notin $names -and $call.Groups[1].Value -ne 'func_80019130') {
            throw "Uncovered transitive animation call: $name -> $($call.Groups[1].Value)"
        }
    }
    $result += $found[$name] + "`n"
}
$output = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output)) | Out-Null
if (-not [System.IO.File]::Exists($output) -or
    [System.IO.File]::ReadAllText($output) -cne $result) {
    [System.IO.File]::WriteAllText($output, $result, [System.Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged generated animation/math functions."
