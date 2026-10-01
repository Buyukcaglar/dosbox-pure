[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RuntimeObjectsDirectory,
    [Parameter(Mandatory = $true)][string]$ZillaLibPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../work/patch-utility-tests'),
    [string]$PatchObject
)
$ErrorActionPreference = 'Stop'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $taskInstallation = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $taskInstallation) { throw 'Visual Studio C++ tools were not found.' }
    Import-Module (Join-Path $taskInstallation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $taskInstallation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

$core = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeObjectsDirectory).Path
$library = (Resolve-Path -LiteralPath $ZillaLibPath).Path
$objects = @(Get-ChildItem -LiteralPath $runtime -Filter '*.obj' | Sort-Object Name | ForEach-Object { $_.FullName })
if (-not ($objects | Where-Object { [IO.Path]::GetFileName($_) -eq 'drive_patch.obj' })) { throw 'Missing drive_patch.obj: build ReleaseGLCORE|x64 first.' }
if ($PatchObject) {
    $replacement = (Resolve-Path -LiteralPath $PatchObject).Path
    $objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne 'drive_patch.obj' }) + $replacement
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$testObject = Join-Path $output 'patch_utility_variant_test.obj'
$executable = Join-Path $output 'patch_utility_variant_test.exe'
& cl.exe /nologo /c /std:c++14 /EHsc /MT /DNDEBUG /D__LIBRETRO__ /DDBP_STANDALONE /D_HAS_EXCEPTIONS=0 /D_CRT_SECURE_NO_WARNINGS "/I$core" "/I$core/include" "/Fo:$testObject" (Join-Path $PSScriptRoot 'patch_utility_variant_test.cpp')
if ($LASTEXITCODE -ne 0) { throw "Patch utility regression compilation failed ($LASTEXITCODE)." }
$response = Join-Path $output 'patch_utility_variant_test.rsp'
$arguments = @('/NOLOGO', '/SUBSYSTEM:CONSOLE', '/ENTRY:wmainCRTStartup', '/OPT:REF', '/LTCG', "/OUT:`"$executable`"", "`"$testObject`"")
$arguments += $objects | ForEach-Object { "`"$_`"" }
$arguments += "`"$library`""
$arguments += @('winmm.lib', 'imm32.lib', 'version.lib', 'opengl32.lib', 'advapi32.lib', 'gdi32.lib', 'user32.lib', 'shell32.lib', 'ws2_32.lib', 'ole32.lib')
Set-Content -LiteralPath $response -Value $arguments -Encoding utf8
& link.exe "@$response"
if ($LASTEXITCODE -ne 0) { throw "Patch utility regression link failed ($LASTEXITCODE)." }
& $executable
if ($LASTEXITCODE -ne 0) { throw "Patch utility regression failed ($LASTEXITCODE)." }
