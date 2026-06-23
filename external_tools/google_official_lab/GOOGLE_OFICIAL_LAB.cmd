@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "TOOL_DIR=%~dp0"
set "ROOT=%TOOL_DIR%..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "CODEX_NODE=%LOCALAPPDATA%\OpenAI\Codex\bin\node.exe"
set "CODEX_PLAYWRIGHT="
set "SCRIPT=%TOOL_DIR%google_official_translate_lab.js"
set "INPUT=%TOOL_DIR%input"
set "OUT=%TOOL_DIR%output"
set "CHROME=C:\Program Files\Google\Chrome\Application\chrome.exe"
set "CDP_URL=http://127.0.0.1:9222"
set "ATLAS_CHROME_PROFILE=%TOOL_DIR%chrome_logged_profile"

if exist "%CODEX_NODE%" (
  set "NODE_EXE=%CODEX_NODE%"
) else (
  set "NODE_EXE=node"
)

for /d %%D in ("%LOCALAPPDATA%\OpenAI\Codex\runtimes\cua_node\*") do (
  if exist "%%~fD\bin\node_modules\playwright" set "CODEX_PLAYWRIGHT=%%~fD\bin\node_modules"
)
set "NODE_PATH=%CODEX_PLAYWRIGHT%"

echo.
echo [Atlas] Google Tradutor oficial
echo [Atlas] Inicio automatico.
echo [Atlas] Usando perfil Chrome persistente do Atlas:
echo [Atlas] %ATLAS_CHROME_PROFILE%
echo [Atlas] Se ainda nao estiver logado nesse perfil, faca login uma vez e rode de novo.
echo [Atlas] Entrada: qualquer .tsv em %INPUT%
echo [Atlas] Saida: %OUT%
echo.

if not exist "%SCRIPT%" (
  echo [ERRO] Script nao encontrado:
  echo %SCRIPT%
  echo.
  pause
  exit /b 1
)

if not exist "%INPUT%" (
  echo [ERRO] Pasta de entrada nao encontrada:
  echo %INPUT%
  echo.
  pause
  exit /b 1
)

set "TSV_COUNT=0"
for %%F in ("%INPUT%\*.tsv") do (
  set "FOUND_TSV=1"
  set /a TSV_COUNT+=1
)

if not defined FOUND_TSV (
  echo [ERRO] Nenhum .tsv encontrado em:
  echo %INPUT%
  echo.
  echo Coloque um arquivo .tsv nessa pasta e rode de novo.
  echo.
  pause
  exit /b 1
)

if not "%TSV_COUNT%"=="1" (
  echo [ERRO] Mais de um .tsv encontrado em:
  echo %INPUT%
  echo.
  echo Deixe apenas um .tsv por vez nessa pasta.
  echo.
  pause
  exit /b 1
)

if "%CODEX_PLAYWRIGHT%"=="" (
  echo [ERRO] Playwright nao encontrado no runtime do Codex:
  echo %LOCALAPPDATA%\OpenAI\Codex\runtimes\cua_node
  echo.
  echo Instale Node.js + Playwright ou ajuste NODE_PATH neste CMD.
  echo.
  pause
  exit /b 1
)

if not exist "%CHROME%" (
  echo [ERRO] Chrome nao encontrado:
  echo %CHROME%
  echo.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $r=Invoke-RestMethod -Uri '%CDP_URL%/json/version' -TimeoutSec 1; if($r.webSocketDebuggerUrl){ exit 0 } } catch {}; exit 1"
set "CHROME_STATE=%ERRORLEVEL%"

if "%CHROME_STATE%"=="0" (
  echo [Atlas] Chrome com porta de controle ja esta ativo.
  goto executar_node
)

if not exist "%ATLAS_CHROME_PROFILE%" mkdir "%ATLAS_CHROME_PROFILE%" >nul 2>nul

echo [Atlas] Abrindo Chrome do Atlas minimizado com porta de controle local...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%CHROME%' -ArgumentList '--remote-debugging-port=9222','--user-data-dir=%ATLAS_CHROME_PROFILE%','--profile-directory=Default','--start-minimized','about:blank' -WindowStyle Minimized"

echo [Atlas] Aguardando Chrome responder em %CDP_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ok=$false; for($i=1;$i -le 30;$i++){ try { $r=Invoke-RestMethod -Uri '%CDP_URL%/json/version' -TimeoutSec 1; if($r.webSocketDebuggerUrl){ $ok=$true; break } } catch {}; Start-Sleep -Seconds 1 }; if(-not $ok){ exit 1 }"
if errorlevel 1 (
  echo [ERRO] Chrome nao abriu a porta de controle 9222.
  echo [ERRO] Feche TODAS as janelas do Chrome e rode este CMD de novo.
  echo [ERRO] Se o Chrome ja estava aberto, ele reaproveita a janela antiga e nao liga o controle.
  echo.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$sig='[DllImport(\"user32.dll\")] public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);'; Add-Type -MemberDefinition $sig -Name Win32ShowWindowAsync -Namespace AtlasWin32; Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | ForEach-Object { [AtlasWin32.Win32ShowWindowAsync]::ShowWindowAsync($_.MainWindowHandle, 6) | Out-Null }"

:executar_node
"%NODE_EXE%" "%SCRIPT%" --input "%INPUT%" --output-dir "%OUT%" --cdp-url "%CDP_URL%" --chrome-profile-directory "Default"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
  echo [Atlas] Concluido.
) else (
  echo [ERRO] Processo terminou com codigo %EXIT_CODE%.
)
echo.
echo [Atlas] Saidas geradas em: %OUT%
echo [Atlas] Arquivos seguem o nome do TSV de entrada.
echo.
pause
exit /b %EXIT_CODE%
