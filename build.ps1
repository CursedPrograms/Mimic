# Builds Mimic and mimic-cli (Release) and dist\Mimic-win64.zip, through build.bat (Visual Studio 2022 + CMake).
$ErrorActionPreference = 'Stop'
& cmd /c "`"$PSScriptRoot\build.bat`""
exit $LASTEXITCODE
