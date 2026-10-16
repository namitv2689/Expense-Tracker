@echo off
REM Windows: double-click this file (needs gcc from MinGW-w64 / MSYS2 on your PATH)
cd /d "%~dp0"
gcc -Wall -Wextra -std=c99 -O2 -o server.exe server.c -lws2_32
if errorlevel 1 (
  echo.
  echo Build failed. Is gcc installed? Run "gcc --version" to check.
  pause
  exit /b 1
)
start "" http://localhost:8080
server.exe
pause
