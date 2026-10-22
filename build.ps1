param([string]$VcpkgRoot = $env:VCPKG_ROOT)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake is missing. Run build.cmd, with Visual Studio C++ tools and CMake installed.'
}
if (-not $VcpkgRoot -and $env:VCPKG_INSTALLATION_ROOT) { $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT }
if (-not $VcpkgRoot) {
    $VcpkgRoot = Join-Path $PSScriptRoot '.deps\vcpkg'
    if (-not (Test-Path "$VcpkgRoot\scripts\buildsystems\vcpkg.cmake")) {
        Run 'git' @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
    }
}
if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) { Run "$VcpkgRoot\bootstrap-vcpkg.bat" @('-disableMetrics') }
Run 'cmake' @('-S', '.', '-B', 'build', '-A', 'x64', "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake", '-DVCPKG_TARGET_TRIPLET=x64-windows-static')
Run 'cmake' @('--build', 'build', '--config', 'Release', '--parallel')
New-Item -ItemType Directory -Force 'dist' | Out-Null
Copy-Item 'build\Release\FrameCap.exe' 'dist\FrameCap.exe' -Force
Copy-Item 'LICENSE', 'THIRD-PARTY-NOTICES.txt' 'dist' -Force
$CopyrightFile = Join-Path $PSScriptRoot 'build\vcpkg_installed\x64-windows-static\share\libvpx\copyright'
if (Test-Path $CopyrightFile) { Copy-Item $CopyrightFile 'dist\libvpx-LICENSE.txt' -Force }
Write-Host 'Built: dist\FrameCap.exe (portable, no runtime installation needed)'
