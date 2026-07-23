#!/usr/bin/env pwsh
# Build the Deadzone Rogue mod DLL (Release, dwmapi.dll proxy).
# Requires: CMake and a Visual Studio C++ toolchain. Vendored deps are git
# submodules, so clone with --recursive (or run: git submodule update --init).

$ErrorActionPreference = 'Stop'

# Locate cmake: prefer PATH, else the copy bundled with Visual Studio.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
             Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw 'cmake not found. Install CMake or Visual Studio with the C++ workload.' }

$mod   = Join-Path $PSScriptRoot 'mod'
$build = Join-Path $mod 'build'

& $cmake -S $mod -B $build
& $cmake --build $build --config Release --target dwmapi

Write-Host "`nBuilt: $build\Release\dwmapi.dll"
