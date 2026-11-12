param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs')
)
$ErrorActionPreference = 'Stop'
$names = @('func_8000F958', 'func_8000F594', 'func_8000F7F4', 'func_8000E65C',
    'func_8000E91C', 'func_8000E780', 'func_8000EC30', 'func_80015A90', 'guMtxF2L',
    'func_8005F15C', 'func_8005F21C', 'func_80013468', 'func_80012CE0',
    'func_80012DBC', 'func_80012BE8', 'func_8001598C', 'func_8001B5B8', '_nsqrtf')
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
$result = "// Exact original object mesh/material and child matrix writers; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    foreach ($call in [regex]::Matches($found[$name], '(?m)^\s+([A-Za-z0-9_]+)\(rdram, ctx\);')) {
        if ($call.Groups[1].Value -notin $names -and $call.Groups[1].Value -ne 'func_8000CD34') {
            throw "Uncovered generated object helper: $name -> $($call.Groups[1].Value)"
        }
    }
    $result += $found[$name] + "`n"
}
$output = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
if (-not [IO.File]::Exists($output) -or [IO.File]::ReadAllText($output) -cne $result) {
    [IO.File]::WriteAllText($output, $result, [Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged original object writers."
