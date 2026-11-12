param([Parameter(Mandatory=$true)][string]$OutputPath,[string]$GeneratedDirectory)
$ErrorActionPreference='Stop'
# A dimension refresh clears the original active-view index. It is safe only
# in audited menu layout/viewport sequences, never the general layout
# helper: race startup can deliberately reuse a view and skip its second half.
$layoutSource=Get-Content (Join-Path $GeneratedDirectory 'funcs_3.c') -Raw
$selectorSource=Get-Content (Join-Path $GeneratedDirectory 'funcs_7.c') -Raw
$layout=[regex]::Match($layoutSource,'(?ms)^RECOMP_FUNC void func_80015C10\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)').Value
$selector=[regex]::Match($selectorSource,'(?ms)^RECOMP_FUNC void func_800247C4\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)').Value
if (-not $layout -or -not $selector) { throw 'Missing viewport integration functions.' }
if ($layout.Contains('rr64_refresh_viewport_dimensions(')) {
    throw 'Unsafe viewport refresh in general layout path: race camera reuse skips viewport setup.'
}
$refresh=$selector.IndexOf('rr64_refresh_viewport_dimensions(rdram, 0u);')
$configure=$selector.IndexOf('func_80015C10(rdram, ctx);')
$viewport=$selector.IndexOf('func_80015CFC(rdram, ctx);')
if ($refresh -lt 0 -or $configure -le $refresh -or $viewport -le $configure) {
    throw 'Menu refresh must precede the original paired layout/viewport setup.'
}
$paired=$selector.Substring($refresh,$viewport-$refresh)
if ($paired -match '\breturn;|if\s*\(') {
    throw 'Menu refresh has a path that can skip the required viewport setup.'
}
# The Bike Shop's separate preview switch uses layout0 only for count=1.
# That positive count guarantees its viewport loop executes. Guard the actual
# generated branch, including the no-view test, rather than allowing a refresh
# at the general preview entry (count=0 deliberately has no viewport rebuild).
$preview=[regex]::Match($selectorSource,'(?ms)^RECOMP_FUNC void func_80024EB8\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)').Value
$single=[regex]::Match($preview,'(?s)L_80024F88:.*?(?=L_80024FC4:)').Value
$join=[regex]::Match($preview,'(?s)L_80025078:.*?func_80015CFC\(rdram, ctx\);').Value
if (-not $preview -or -not $single -or -not $join -or
    ([regex]::Matches($preview,'rr64_refresh_viewport_dimensions\(')).Count -ne 1 -or
    -not $preview.Contains('ctx->r23 = ADD32(ctx->r4, 0);') -or
    -not $preview.Contains('case 2: goto L_80024F88;') -or
    -not $single.Contains('rr64_refresh_viewport_dimensions(rdram, 0u);') -or
    $single.IndexOf('rr64_refresh_viewport_dimensions(') -ge $single.IndexOf('func_80015C10(rdram, ctx);') -or
    -not $single.Contains('goto L_80025078;') -or
    $single -match '\breturn;|if\s*\(|ctx->r23\s*=' -or
    -not $join.Contains('if (SIGNED(ctx->r23) <= 0)') -or
    $join -match 'ctx->r23\s*=') {
    throw 'Bike preview refresh must stay inside count=1 and reach its viewport rebuild.'
}
$source=Get-Content (Join-Path $GeneratedDirectory 'funcs_0.c') -Raw
$result="#include `"recomp.h`"`n"
foreach($name in @('func_8000A310','func_8000A35C','func_8000A3A8')) {
    $match=[regex]::Match($source,'(?ms)^RECOMP_FUNC void '+$name+'\([^\n]+\) \{.*?(?=^RECOMP_FUNC|\z)')
    if(-not $match.Success){throw "Missing $name"}
    $result+=$match.Value+"`n"
}
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputPath))|Out-Null
[IO.File]::WriteAllText($OutputPath,$result)
