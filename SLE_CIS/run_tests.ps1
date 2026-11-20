param([string]$Compiler = 'D:\vivado\Vitis_HLS\2020.2\tps\win64\msys64\mingw64\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
$cc = (Get-Command $Compiler -CommandType Application -ErrorAction Stop).Source
$oldPath = $env:Path
$binary = Join-Path ([IO.Path]::GetTempPath()) ('sle-cis-' + [guid]::NewGuid().ToString('N') + '.exe')
Push-Location $PSScriptRoot
try {
    $env:Path = (Split-Path -Parent $cc) + ';' + $oldPath
    & $cc --version | Select-Object -First 1
    $sources = @((Get-ChildItem -File -Filter '*.c').Name)
    $sources += @('../SLE_BIS/sle_sch.c','../SLE_BIS/sle_controller.c','../SLE_BIS/sle_codec.c','../SLE_BIS/sle_adv.c','../SLE_BIS/sle_fpga.c')
    & $cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror @sources -o $binary
    if ($LASTEXITCODE) { throw "C11 build failed ($LASTEXITCODE)" }
    & $binary
    if ($LASTEXITCODE) { throw "CIS regression failed ($LASTEXITCODE)" }
} finally {
    if (Test-Path -LiteralPath $binary) { Remove-Item -LiteralPath $binary }
    $env:Path = $oldPath
    Pop-Location
}
