param([string]$Tag = 'menu', [string[]]$Modes = @('base','clamp','mip0','pp','mip0+pp','clamp+mip0+pp','nomip','point'), [switch]$MeasureOnly)
$ErrorActionPreference = 'Stop'
$dir = 'E:\Temp\claude\E--DEV-PhoenixPoint-Renderforge\0465ee46-0a6d-418e-8f52-fa297da8064f\scratchpad\bleed'
Set-Location E:\DEV\PhoenixPoint\PPCLI
function Invoke-RF($member, $a) {
    $j = @{op='invoke';type='Renderforge.IconBleed';assembly='Renderforge';member=$member;args=@($a)} | ConvertTo-Json -Compress
    $r = .\ppcli.ps1 connect call $j 2>$null | ConvertFrom-Json
    if (-not $r.result.ok) { throw "call $member failed: $($r | ConvertTo-Json -Compress)" }
    return $r.result.value
}
$resPath = Join-Path $dir "$Tag-results.json"
$rows = @{}
if ($MeasureOnly) {
    $saved = Get-Content $resPath | ConvertFrom-Json
    foreach ($m in $Modes) { $rows[$m] = @{info=$saved.$m.info; rects=$saved.$m.rects} }
} else {
    foreach ($m in $Modes) {
        $info = Invoke-RF 'Mode' @($m)
        .\ppcli.ps1 connect wait '{"forMs":150}' 2>$null | Out-Null
        $png = Join-Path $dir "$Tag-$($m.Replace('+','_')).png"
        $s = .\ppcli.ps1 connect screenshot (@{path=$png} | ConvertTo-Json -Compress) 2>$null | ConvertFrom-Json
        if (-not $s.result.ok) { throw "screenshot failed $($s | ConvertTo-Json -Compress)" }
        $rects = Invoke-RF 'Rects' @()
        $rows[$m] = @{info=$info; rects=$rects}
        Write-Host "$m => $info"
    }
    $rows | ConvertTo-Json -Depth 6 | Set-Content $resPath
}
Write-Host ("{0,-16} {1,-16} {2,-16} {3,-16}" -f "mode [$Tag]", 'atlas .0/.33/.66', 'fullrect T|R', 'vanilla edgeLum')
foreach ($m in $Modes) {
    $png = Join-Path $dir "$Tag-$($m.Replace('+','_')).png"
    $meas = python (Join-Path $dir 'measure.py') $png $rows[$m].rects | ConvertFrom-Json
    $rows[$m].meas = $meas
    $line = "{0,-16}" -f $m
    foreach ($kind in 'atlas','fullrect','vanilla') {
        foreach ($f in '0.00','0.33','0.66') {
            $e = $meas."$kind`_$f"
            if ($kind -eq 'fullrect') { $worst = [Math]::Max($e.T, $e.R) } else { $worst = ($e.L, $e.R, $e.T, $e.B | Measure-Object -Maximum).Maximum }
            $line += " {0,4}" -f $worst
        }
        $line += " |"
    }
    Write-Host $line
}
$rows | ConvertTo-Json -Depth 6 | Set-Content $resPath
