param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs')
)
$ErrorActionPreference = 'Stop'
$names = @('func_80011CC0', 'func_80015A90', 'func_80015834', 'func_80015040',
    'func_800150AC', 'func_800150E8', 'func_80012CE0', 'func_80012DBC',
    'func_80013468', 'func_80012D3C', 'func_8001A634', 'func_8001F700',
    'func_8005E980', 'func_8005EB50', 'func_800167BC', 'func_80019F7C', 'func_80014178',
    'n_alSeqpDelete', 'guMtxF2L', 'guPerspectiveF', 'guPerspective',
    'guLookAt', 'guMtxIdentF', '_nsqrtf', '_bcopy')
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
$result = "// Generated test input: exact current recompiled functions; do not edit.`n" +
    "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n`n"
foreach ($name in $names) {
    if (-not $found.ContainsKey($name)) { throw "Missing generated function: $name" }
    $result += $found[$name] + "`n"
}
$output = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($output)) | Out-Null
# Keep the generated source's timestamp when extraction is unchanged, so a
# dependency update does not force an otherwise identical C translation unit.
if (-not [System.IO.File]::Exists($output) -or
    [System.IO.File]::ReadAllText($output) -cne $result) {
    [System.IO.File]::WriteAllText($output, $result, [System.Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($names.Count) unchanged generated renderer/math functions."
