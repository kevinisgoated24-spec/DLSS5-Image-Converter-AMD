@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
set EC=%ERRORLEVEL%
echo.
pause
exit /b %EC%
