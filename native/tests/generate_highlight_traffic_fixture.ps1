param([Parameter(Mandatory=$true)][string]$OutputPath,
      [string]$GeneratedDirectory = (Join-Path $PSScriptRoot '../../build/RecompiledFuncs'))
$ErrorActionPreference='Stop'
$bodies=@{}
foreach($file in Get-ChildItem -LiteralPath $GeneratedDirectory -Filter '*.c') {
    $source=[IO.File]::ReadAllText($file.FullName)
    foreach($match in [regex]::Matches($source,'(?ms)^RECOMP_FUNC void (\w+)\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)')) {
        $bodies[$match.Groups[1].Value]=$match.Value
    }
}
$queue=[Collections.Generic.Queue[string]]::new()
@('func_8001A69C','func_8001A994','func_80011988','func_8001B8F4','func_8007B254',
  'func_8005EF74','func_8000F958','func_80015A90','guMtxF2L','func_800464B8',
  'func_8006E3D4','func_8001BA88') | ForEach-Object { $queue.Enqueue($_) }
# Instrumented allocator backing and forbidden ROM I/O only. Native graph
# construction, list sharing, destruction and resource expiry execute exactly.
$boundaries=@('func_8001BDF8','func_8001C084','func_8000CD34')
$selected=@{}
while($queue.Count) {
    $name=$queue.Dequeue()
    if($selected.ContainsKey($name) -or $name -in $boundaries) { continue }
    if(-not $bodies.ContainsKey($name)) { throw "Missing exact native function $name" }
    $selected[$name]=$bodies[$name]
    foreach($call in [regex]::Matches($bodies[$name],'(?m)^\s+(\w+)\(rdram, ctx\);')) {
        $queue.Enqueue($call.Groups[1].Value)
    }
}
$source=$bodies['func_8006AFFC']
$start=$source.IndexOf('    // 0x8006B43C:')
$end=$source.IndexOf('L_8006B5A4:', $start)
if($start -lt 0 -or $end -lt $start) { throw 'Native traffic retirement boundaries changed' }
$retire=$source.Substring($start,$end-$start)
$retire=[regex]::Replace($retire,'(?m)^\s*rr64_online_traffic_finish\([^\n]*\n','')
$text="// Exact native constructors, materials, retirement, destruction and expiry.`n"+
      "#include `"recomp.h`"`n#include `"rr64_native.hpp`"`n#include `"funcs.h`"`n"+
      "void rr64_traffic_fixture_retire(uint8_t* rdram,recomp_context* ctx) {`n"+
      "    uint64_t hi=0,lo=0,result=0; int c1cs=0;`n"+$retire+"L_8006B5A4:;`n}`n"
foreach($name in ($selected.Keys | Sort-Object)) { $text+=$selected[$name]+"`n" }
$path=[IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
if(-not [IO.File]::Exists($path) -or [IO.File]::ReadAllText($path) -cne $text) {
    [IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))
}
Write-Output "Extracted $($selected.Count) exact native helpers and traffic retirement block 6B43C..6B5A4."
