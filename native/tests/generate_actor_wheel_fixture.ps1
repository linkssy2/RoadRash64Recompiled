param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs'),
    [switch]$OnlyMath
)
$ErrorActionPreference = 'Stop'
$names = @('func_8005B948', 'func_80015834', 'func_80015040', 'func_800150AC',
    'func_800150E8', 'func_8001A634', 'func_80012D3C', 'func_8001A250',
    'func_8001B020', 'func_80013468', '__cosf_recomp', '__sinf_recomp', '_nsqrtf')
if ($OnlyMath) { $names = @($names | Where-Object { $_ -ne 'func_8005B948' }) }
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
$result = "// Generated test oracle: exact wheel writer and arithmetic; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    $result += $found[$name] + "`n"
}
$output = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output)) | Out-Null
if (-not [System.IO.File]::Exists($output) -or
    [System.IO.File]::ReadAllText($output) -cne $result) {
    [System.IO.File]::WriteAllText($output, $result, [System.Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged generated wheel/math functions."
