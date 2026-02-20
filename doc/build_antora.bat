@echo off
setlocal

if "%~1"=="" (
    echo No playbook supplied, using default playbook
    set "PLAYBOOK=local-playbook.yml"
) else (
    set "PLAYBOOK=%~1"
)

echo Building documentation with Antora...
echo Installing npm dependencies...
call npm ci

echo Building docs in custom dir...
call "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set "PATH=%PATH%;C:\Program Files\7-Zip"
set "PATH=%PATH%;%CD%\node_modules\.bin"
call npx antora --clean --fetch "%PLAYBOOK%"
echo Done
