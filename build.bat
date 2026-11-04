@echo off
REM ---------------------------------------------------------------
REM build.bat -- one-click build for Windows (MinGW-w64 / TDM-GCC)
REM   build.bat          compile server + client into build\
REM   build.bat clean    remove build\
REM
REM NOTE: messages are kept in ASCII on purpose, because cmd.exe
REM       reads this file using the OEM code page (GBK on zh-CN),
REM       and UTF-8 Chinese would be displayed as garbage.
REM ---------------------------------------------------------------
setlocal
cd /d "%~dp0"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    echo [clean] build\ removed
    exit /b 0
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found in PATH.
    echo         Install MinGW-w64 ^(or TDM-GCC^) and add its bin dir to PATH.
    exit /b 1
)

if not exist build mkdir build

REM Remove leftovers of a previous swap. This succeeds as soon as the
REM process that was still running the old binary has exited.
del /f /q build\*.old.exe >nul 2>nul

REM The binaries are compiled to a *.new.exe first and then swapped in.
REM Reason: while the demo is running, chat_server.exe / chat_client.exe
REM are locked by the OS (a running image cannot be overwritten), and the
REM linker would fail with "cannot open output file ... Permission denied".
REM Renaming a running image IS allowed, so the old file is moved aside
REM and only then the new one is put in place.

echo [1/2] compiling chat_server.exe ...
g++ -std=c++11 -Wall -Wextra -O2 -o build\chat_server.new.exe src\server.cpp src\net_io.cpp src\protocol.cpp -lws2_32
if errorlevel 1 (
    echo [ERROR] server build failed
    exit /b 1
)
call :swap chat_server

echo [2/2] compiling chat_client.exe ...
g++ -std=c++11 -Wall -Wextra -O2 -o build\chat_client.new.exe src\client.cpp src\net_io.cpp src\protocol.cpp -lws2_32
if errorlevel 1 (
    echo [ERROR] client build failed
    exit /b 1
)
call :swap chat_client

echo [OK] build complete: build\chat_server.exe  build\chat_client.exe
exit /b 0

REM %1 = base name of the executable (without extension)
:swap
if exist build\%1.exe move /Y build\%1.exe build\%1.old.exe >nul 2>nul
move /Y build\%1.new.exe build\%1.exe >nul
if exist build\%1.old.exe del /f /q build\%1.old.exe >nul 2>nul
if exist build\%1.old.exe echo [WARN] build\%1.old.exe is still running, it will be removed on the next build.
exit /b 0
