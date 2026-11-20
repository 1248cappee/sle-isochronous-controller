param(
    [string]$VivadoBin = 'D:\vivado\Vivado\2020.2\bin',
    [string]$Compiler = 'D:\vivado\Vitis_HLS\2020.2\tps\win64\msys64\mingw64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$arch = $env:PROCESSOR_ARCHITECTURE
$work = Join-Path ([IO.Path]::GetTempPath()) ("sle_bis_xsim_" + [guid]::NewGuid().ToString('N'))
$results = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Path $work | Out-Null
$runner = Join-Path (Split-Path $PSScriptRoot) 'run_tests.ps1'
$trace = @(& $runner -Compiler $Compiler)
if ($trace -notcontains 'ALL TESTS PASSED') { throw 'C regression did not pass' }
$events = @(foreach ($line in $trace) {
    if ($line -match '^EVENT token=(\d+) us=(\d+) slot=(\d+) phy_channel=(\d+) accepted_pdus=\d+$') {
        $Matches[1..4] -join ' '
    }
})
if ($events.Count -ne 4) { throw 'Expected four C descriptors' }
Set-Content -LiteralPath (Join-Path $work 'events.txt') -Encoding ascii -Value $events
foreach ($name in 'bis_event_window.sv','tb_bis_event_window.sv','run.tcl') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $work
}
Push-Location $work
try {
    if (-not $env:PROCESSOR_ARCHITECTURE) { $env:PROCESSOR_ARCHITECTURE='AMD64' }
    & (Join-Path $VivadoBin 'xvlog.bat') --sv bis_event_window.sv tb_bis_event_window.sv
    if ($LASTEXITCODE) { throw 'xvlog failed' }
    & (Join-Path $VivadoBin 'xelab.bat') work.tb_bis_event_window --debug typical --snapshot bis_sim
    if ($LASTEXITCODE) { throw 'xelab failed' }
    & (Join-Path $VivadoBin 'xsim.bat') bis_sim --tclbatch run.tcl --wdb bis_event_window.wdb
    if ($LASTEXITCODE -or !(Select-String -LiteralPath 'xsim.log' -SimpleMatch 'BIS_XSIM_PASS starts=6 completions=8' -Quiet)) {
        throw "Simulation did not pass. Diagnostic files: $work"
    }
    New-Item -ItemType Directory -Path $results -Force | Out-Null
    foreach ($name in 'xvlog.log','xelab.log','xsim.log','events.txt','bis_event_window.wdb','bis_event_window.wcfg','bis_event_window.vcd') {
        if ((Get-Item -LiteralPath $name).Length -eq 0) { throw "Empty result: $name" }
        Copy-Item -LiteralPath $name -Destination $results -Force
    }
} finally {
    Pop-Location
    $env:PROCESSOR_ARCHITECTURE=$arch
}
