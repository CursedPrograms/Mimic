# Installs Mimic for this user (%LOCALAPPDATA%\Programs\Mimic, Start menu + Desktop shortcuts), through install.bat.
& cmd /c "`"$PSScriptRoot\install.bat`" $args"
exit $LASTEXITCODE
