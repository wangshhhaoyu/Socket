@echo off
REM ---------------------------------------------------------------
REM demo.bat -- offline live demo launcher (Windows)
REM   Opens 1 server window + 3 client windows (pc1 / pc2 / pc3).
REM   The clients do NOT pass -n, so the server assigns the default
REM   names pc1 / pc2 / pc3 in connection order.
REM   Type a message in any client window; every other window
REM   receives it immediately.
REM
REM   demo.bat          normal demo, no artificial packet loss
REM   demo.bat 30       inject 30 percent artificial loss (server side)
REM
REM NOTE: messages are kept in ASCII on purpose, because cmd.exe
REM       reads this file using the OEM code page (GBK on zh-CN),
REM       and UTF-8 Chinese would be displayed as garbage.
REM ---------------------------------------------------------------
setlocal
cd /d "%~dp0"

if not exist build\chat_server.exe (
    echo [INFO] binaries missing, building now ...
    call build.bat
    if errorlevel 1 exit /b 1
)

set PORT=8888
set DROP=0
if not "%~1"=="" set DROP=%~1

start "CHT-Server" cmd /k "build\chat_server.exe -p %PORT% --drop %DROP%"
REM Wait for the server to finish binding before starting the clients.
REM Use ping, not timeout: Git Bash ships /usr/bin/timeout which shadows
REM the built-in timeout.exe, so the delay would fail and the clients
REM would race the server and fail to connect.
ping -n 3 127.0.0.1 >nul

REM No -n here on purpose: the server assigns pc1 / pc2 / pc3 by connection order.
start "CHT-pc1" cmd /k "build\chat_client.exe -h 127.0.0.1 -p %PORT%"
start "CHT-pc2" cmd /k "build\chat_client.exe -h 127.0.0.1 -p %PORT%"
start "CHT-pc3" cmd /k "build\chat_client.exe -h 127.0.0.1 -p %PORT%"

echo.
echo   Server + 3 clients started (names auto-assigned: pc1 / pc2 / pc3).
echo   - type text in any client window to broadcast to all
echo   - /who        list online users
echo   - /ping       measure round-trip latency
echo   - /test 100   measure application-level packet loss
echo   - /stat       show local statistics
echo   - /quit       leave the chat room
echo.
echo   Close the server window to stop the demo.
endlocal
