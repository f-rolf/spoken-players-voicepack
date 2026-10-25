@echo off
rem Builds the voicepack's installer into out\ (the setup, and voices.txt naming its downloads):
rem double-click it. tools\Build-Installer.ps1 says what goes in (and -Url, -Packs to build another).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Build-Installer.ps1" %* & echo. & pause & exit /b
