@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "TOOL_DIR=%~dp0"
set "CHROME=C:\Program Files\Google\Chrome\Application\chrome.exe"
set "ATLAS_CHROME_PROFILE=%TOOL_DIR%chrome_logged_profile"

if not exist "%ATLAS_CHROME_PROFILE%" mkdir "%ATLAS_CHROME_PROFILE%" >nul 2>nul

echo [Atlas] Abrindo perfil Chrome persistente do Atlas para login.
echo [Atlas] Faca login no Google se quiser usar o Google Tradutor logado.
echo [Atlas] Depois feche o Chrome e rode GOOGLE_OFICIAL_LAB.cmd.
echo.

start "" "%CHROME%" --user-data-dir="%ATLAS_CHROME_PROFILE%" --profile-directory="Default" "https://translate.google.com/?sl=pt&tl=en&op=translate"
