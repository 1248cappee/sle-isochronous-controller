param(
    [string]$VivadoBin = 'D:\vivado\Vivado\2020.2\bin',
    [string]$Compiler = 'D:\vivado\Vitis_HLS\2020.2\tps\win64\msys64\mingw64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$arch = $env:PROCESSOR_ARCHITECTURE
$runner = Join-Path (Split-Path $PSScriptRoot -Parent) 'run_tests.ps1'
$trace = @(& $runner -Compiler $Compiler)
if ($trace -notcontains 'ALL TESTS PASSED') { throw 'C regression did not pass' }
function Get-Events([string]$prefix) {
    $pattern = '^' + $prefix + ' token=(\d+) first_us=(\d+) second_us=(\d+) end_us=(\d+) slot=(\d+) channel=(\d+) first_tx=(\d+) first_air_us=(\d+)$'
    foreach ($line in $trace) {
        if ($line -match $pattern) { $Matches[1..8] -join ' ' }
    }
}
$events = @(Get-Events 'CIS_EVENT')
$linkEvents = @(Get-Events 'CIS_LINK_EVENT')
if ($events.Count -ne 4) { throw 'Expected four A-side C descriptors' }
if ($linkEvents.Count -ne 3) { throw 'Expected three negotiated endpoint descriptors' }
$work = Join-Path ([IO.Path]::GetTempPath()) ("sle_cis_xsim_" + [guid]::NewGuid().ToString('N'))
$results = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Path $work | Out-Null
Set-Content -LiteralPath (Join-Path $work 'events.txt') -Encoding ascii -Value $events
Set-Content -LiteralPath (Join-Path $work 'e2e_events.txt') -Encoding ascii -Value $linkEvents
foreach ($name in 'cis_paired_window.sv','tb_cis_paired_window.sv','run.tcl') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $work
}
Push-Location $work
try {
    if (-not $env:PROCESSOR_ARCHITECTURE) { $env:PROCESSOR_ARCHITECTURE='AMD64' }
    & (Join-Path $VivadoBin 'xvlog.bat') --sv cis_paired_window.sv tb_cis_paired_window.sv
    if ($LASTEXITCODE) { throw "xvlog failed: $work" }
    & (Join-Path $VivadoBin 'xelab.bat') work.tb_cis_paired_window --debug typical --snapshot cis_sim
    if ($LASTEXITCODE) { throw "xelab failed: $work" }
    & (Join-Path $VivadoBin 'xsim.bat') cis_sim --tclbatch run.tcl --wdb cis_paired_window.wdb
    if ($LASTEXITCODE -or !(Select-String -LiteralPath 'xsim.log' -SimpleMatch 'CIS_XSIM_PASS phases=18 completions=12' -Quiet)) {
        throw "CIS XSim failed: $work"
    }
    New-Item -ItemType Directory -Path $results -Force | Out-Null
    foreach ($name in 'xvlog.log','xelab.log','xsim.log','events.txt','e2e_events.txt','cis_paired_window.wdb','cis_paired_window.wcfg','cis_paired_window.vcd') {
        if ((Get-Item -LiteralPath $name).Length -eq 0) { throw "Empty result: $name" }
        Copy-Item -LiteralPath $name -Destination $results -Force
    }
} finally {
    Pop-Location
    $env:PROCESSOR_ARCHITECTURE=$arch
}
