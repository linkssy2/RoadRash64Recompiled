param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs')
)
$ErrorActionPreference = 'Stop'
$names = @('func_8007CFFC', 'func_80010FD0', 'func_8000E65C', 'func_8000E91C',
    'func_8000E780', 'func_80010640', 'func_8007D814', 'func_80015A90',
    'n_alSeqpDelete', 'guMtxF2L')
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
$result = "// Exact original terrain packet and material writers; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    $result += $found[$name] + "`n"
}
foreach ($call in [regex]::Matches($result, '(func_[0-9A-Fa-f]+)\(rdram, ctx\)')) {
    if ($call.Groups[1].Value -notin $names -and $call.Groups[1].Value -ne 'func_8000CD34') {
        throw "Uncovered generated terrain helper: $($call.Groups[1].Value)"
    }
}
$output = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
if (-not [IO.File]::Exists($output) -or [IO.File]::ReadAllText($output) -cne $result) {
    [IO.File]::WriteAllText($output, $result, [Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged original terrain packet/material functions."
