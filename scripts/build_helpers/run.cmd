@echo off
rem ------------------------------------------------------------------------------------------------------------------
rem run.cmd - start the renderer WITHOUT going through the shell's app-reputation check.
rem
rem WHY THIS FILE EXISTS AT ALL, when every other script in this directory is PowerShell: the prompt this avoids is
rem raised by ShellExecuteEx, which is what a DOUBLE-CLICK on an .exe (and PowerShell's `Start-Process`) uses. An
rem .exe carries no signature here - it is built on this machine, for this machine - so Windows asks about the
rem "unknown publisher" on every launch, and a capture batch launches it dozens of times. A .cmd is not an image:
rem the shell runs the Microsoft-signed `cmd.exe`, and the line below starts the renderer with CreateProcess, which
rem has no such check, which is the point: a capture batch launches the renderer dozens of times.
rem
rem ARGUMENTS ARE FORWARDED, so this is also the way to run a capture by hand:
rem     run.cmd run source\scripting\test_game --capture-frames 60 --capture-camera=0,0,2.4,0,-0.9,0
rem The working directory becomes the build directory and the project path is resolved from it, so the
rem project is named WITHOUT the leading `build\`: the test project's copy lives at
rem `<build>\source\scripting\test_game`, i.e. `build\build\...` from the repository root.
rem ------------------------------------------------------------------------------------------------------------------
setlocal
set "REPO=%~dp0..\.."
set "BUILD=%REPO%\build"
if not exist "%BUILD%\deren.exe" (
    echo run.cmd: "%BUILD%\deren.exe" does not exist - build it first:
    echo     cmake -S "%REPO%" -B "%BUILD%"
    echo     cmake --build "%BUILD%"
    exit /b 1
)
pushd "%BUILD%"
"%BUILD%\deren.exe" %*
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
