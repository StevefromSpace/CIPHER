# Configure, build, and test CIPHER out of source (Release, Ninja), the same steps as CI.
# Build output goes to -BuildDir (outside the repository) so the source tree stays clean.
# If cl.exe is not on PATH, the MSVC x64 environment is loaded from vcvars64.bat and
# the Ninja bundled with VS 2022 Build Tools is added to PATH.
param(
    [string]$BuildDir = (Join-Path $env:LOCALAPPDATA 'cipher-build-ninja')
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars. Run from a Developer shell." }
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
}
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    $env:PATH += ';' + (Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
}

cmake -S $root -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "CTest failed (exit $LASTEXITCODE)" }

Write-Host "CIPHER verify passed. Build directory: $BuildDir"
