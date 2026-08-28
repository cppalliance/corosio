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

rem The reference examples are injected by addons\extensions\reference-snippets.lua.
rem MrDocs loads extensions only from <install>\share\mrdocs\addons\extensions, so
rem the extension has to be placed inside a MrDocs install. Unlike build_antora.sh
rem this script does not download MrDocs; it only installs the extension into an
rem install the caller supplies through MRDOCS_ROOT. Without it the reference
rem builds with no examples at all and still reports success.
if defined MRDOCS_ROOT (
    if not exist "%MRDOCS_ROOT%\share\mrdocs\addons\extensions" (
        mkdir "%MRDOCS_ROOT%\share\mrdocs\addons\extensions"
    )
    copy /Y "addons\extensions\*.lua" "%MRDOCS_ROOT%\share\mrdocs\addons\extensions" >nul
    echo MrDocs: %MRDOCS_ROOT% ^(reference-snippets extension installed^)
) else (
    echo WARNING: MRDOCS_ROOT is not set, so reference-snippets.lua cannot be
    echo WARNING: installed and the reference will render with no examples.
    echo WARNING: Set MRDOCS_ROOT to a MrDocs develop install to inject them.
)

echo Building docs in custom dir...
call "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set "PATH=%PATH%;C:\Program Files\7-Zip"
set "PATH=%PATH%;%CD%\node_modules\.bin"
call npx antora --clean --fetch "%PLAYBOOK%"
echo Done
