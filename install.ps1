# Installs Mimic for this user (%LOCALAPPDATA%\Programs\Mimic, Start menu and Desktop shortcuts named "Atelier - Mimic"), through install.bat.
& cmd /c "`"$PSScriptRoot\install.bat`" $args"
exit $LASTEXITCODE
