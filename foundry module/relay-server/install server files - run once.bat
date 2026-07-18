@echo off
call npm install
if errorlevel 1 (
    echo npm install failed
    pause
    exit /b 1
)
pause