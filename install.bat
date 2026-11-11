@echo off
setlocal
cd /d "%~dp0"
rem Installs Mimic for this user: %LOCALAPPDATA%\Programs\Mimic, with Start menu and Desktop shortcuts.
rem LayerTack finds it there for Filter > Mimic. Run build.bat first (or unzip a release and run this beside it).
set "SRC=dist\Mimic"
if exist "Mimic.exe" set "SRC=."
if not exist "%SRC%\Mimic.exe" ( echo Build it first: build.bat & exit /b 1 )
tasklist /fi "imagename eq Mimic.exe" | find /i "Mimic.exe" >nul
if not errorlevel 1 ( echo Mimic is open: close it and run this again. & exit /b 1 )
set "INST=%LOCALAPPDATA%\Programs\Mimic"
if not exist "%INST%" mkdir "%INST%"
xcopy /e /i /q /y "%SRC%\*" "%INST%\" >nul || ( echo Couldn't copy to %INST% & exit /b 1 )
powershell -NoProfile -Command "$s=New-Object -ComObject WScript.Shell; foreach($d in @([Environment]::GetFolderPath('Programs'),[Environment]::GetFolderPath('Desktop'))){ $l=$s.CreateShortcut((Join-Path $d 'Mimic.lnk')); $l.TargetPath='%INST%\Mimic.exe'; $l.WorkingDirectory='%INST%'; $l.IconLocation='%INST%\Mimic.exe,0'; $l.Description='Mimic: one face onto a picture or a video'; $l.Save() }"
echo Installed: %INST%
if /i not "%~1"=="--quiet" echo Start it from the Start menu or the Desktop.
exit /b 0
