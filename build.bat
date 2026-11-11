@echo off
setlocal
cd /d "%~dp0"
rem Builds Mimic (the window) and mimic-cli (the command line) into build\Release, and dist\Mimic-win64.zip.
rem Needs Visual Studio 2022 (C++) and CMake. SDL2 and ONNX Runtime are fetched into external\ once.
if not exist "external\SDL2-2.30.9\cmake" (
    echo Getting SDL2's development package, once...
    if not exist external mkdir external
    curl -sL -o external\SDL2-devel.zip https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip || goto :error
    tar -xf external\SDL2-devel.zip -C external || goto :error
    del external\SDL2-devel.zip
)
if not exist "external\ortdml\DirectML.dll" (
    echo Getting ONNX Runtime with DirectML, once...
    if not exist external\ortdml mkdir external\ortdml
    curl -sL -o external\ort.nupkg https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1 || goto :error
    curl -sL -o external\dml.nupkg https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4 || goto :error
    mkdir external\nug 2>nul
    tar -xf external\ort.nupkg -C external\nug runtimes/win-x64/native/onnxruntime.dll build/native/include || goto :error
    tar -xf external\dml.nupkg -C external\nug bin/x64-win/DirectML.dll || goto :error
    copy /y external\nug\runtimes\win-x64\native\onnxruntime.dll external\ortdml\ >nul
    copy /y external\nug\bin\x64-win\DirectML.dll external\ortdml\ >nul
    copy /y external\nug\build\native\include\*.h external\ortdml\ >nul
    rmdir /s /q external\nug
    del external\ort.nupkg external\dml.nupkg
)
cmake -B build -G "Visual Studio 17 2022" -A x64 || goto :error
cmake --build build --config Release || goto :error
if exist dist\Mimic rmdir /s /q dist\Mimic
mkdir dist\Mimic
for %%f in (Mimic.exe mimic-cli.exe SDL2.dll onnxruntime.dll DirectML.dll) do copy /y build\Release\%%f dist\Mimic\ >nul
xcopy /e /i /q /y build\Release\models dist\Mimic\models >nul
xcopy /e /i /q /y build\Release\assets dist\Mimic\assets >nul
copy /y LICENSE dist\Mimic\ >nul
copy /y NOTICE dist\Mimic\ >nul
copy /y README.md dist\Mimic\ >nul
copy /y install.bat dist\Mimic\ >nul
copy /y install.ps1 dist\Mimic\ >nul
if exist dist\Mimic-win64.zip del dist\Mimic-win64.zip
"%SystemRoot%\System32\tar.exe" -a -c -f dist\Mimic-win64.zip -C dist Mimic || goto :error
echo Built: build\Release\Mimic.exe, mimic-cli.exe and dist\Mimic-win64.zip
rem installed on this PC: this build replaces that copy too
if exist "%LOCALAPPDATA%\Programs\Mimic\Mimic.exe" call "%~dp0install.bat" --quiet
exit /b 0
:error
echo Build failed, see above.
pause
exit /b 1
