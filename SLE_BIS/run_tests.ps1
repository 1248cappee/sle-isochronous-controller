param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$cc = (Get-Command $Compiler -CommandType Application -ErrorAction Stop).Source
$previousPath = $env:Path
$binary = Join-Path ([IO.Path]::GetTempPath()) ("sle-bis-{0}.exe" -f [guid]::NewGuid())
Push-Location $PSScriptRoot
try {
    $env:Path = (Split-Path $cc) + ';' + $previousPath
    & $cc --version | Select-Object -First 1
    & $cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror sle_controller.c sle_adv.c sle_codec.c sle_sch.c sle_fpga.c test_bis.c -o $binary
    if ($LASTEXITCODE -ne 0) { throw "C11 build failed ($LASTEXITCODE)" }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Regression failed ($LASTEXITCODE)" }
} finally {
    if (Test-Path -LiteralPath $binary) { Remove-Item -LiteralPath $binary }
    $env:Path = $previousPath
    Pop-Location
}
