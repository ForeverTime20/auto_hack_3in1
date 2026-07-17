$ErrorActionPreference = "Stop"

$src = @(
  "$PSScriptRoot\src\main.cpp",
  "$PSScriptRoot\src\games\slider_module.cpp",
  "$PSScriptRoot\src\games\flashing_module.cpp",
  "$PSScriptRoot\src\games\fingerprint_module.cpp"
)

$out = Join-Path $PSScriptRoot "auto_hack_3in1.exe"

$gxx = $env:CXX
if (-not $gxx) {
  $defaultGxx = "C:\mingw64\bin\g++.exe"
  if (Test-Path -LiteralPath $defaultGxx) {
    $gxx = $defaultGxx
  } else {
    $cmd = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($cmd) { $gxx = $cmd.Source }
  }
}

if (-not $gxx) {
  throw "g++.exe not found. Install MinGW-w64, add it to PATH, or set CXX to the compiler path."
}

& $gxx `
  -std=c++17 `
  -O2 `
  -static `
  -static-libgcc `
  -static-libstdc++ `
  -municode `
  -mwindows `
  $src `
  -lgdi32 `
  -luser32 `
  -lshell32 `
  -lcomctl32 `
  -ldwmapi `
  -o $out

Remove-Item -LiteralPath (Join-Path $PSScriptRoot "gta_3in1.exe") -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $PSScriptRoot "Auto Hack 3in1.exe") -Force -ErrorAction SilentlyContinue
