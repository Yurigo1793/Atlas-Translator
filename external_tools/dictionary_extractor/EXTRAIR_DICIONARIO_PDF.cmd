@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "TOOL_DIR=%~dp0"
set "ROOT=%TOOL_DIR%..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "PDF=%ROOT%\external_tools\dicionario-pt.pdf"
set "SCRIPT=%TOOL_DIR%extract_dictionary_pdf.py"
set "OUT_DIR=%TOOL_DIR%output"
set "OUT=%OUT_DIR%\dicionario_pt_extraido.tsv"
set "MARKS=%OUT_DIR%\dicionario_pt_extraido_marcas_abreviacoes.tsv"
set "PYTHON=%LOCALAPPDATA%\Programs\Python\Python314\python.exe"
set "PYTHONPATH=%TOOL_DIR%.venv\Lib\site-packages"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%" >nul 2>nul

echo [Atlas] Extrator de dicionario PDF
echo [Atlas] PDF: %PDF%
echo [Atlas] Saida: %OUT%
echo [Atlas] Marcas: %MARKS%
echo.

if not exist "%PDF%" (
  echo [ERRO] PDF nao encontrado.
  pause
  exit /b 1
)

if not exist "%PYTHON%" (
  echo [ERRO] Python nao encontrado:
  echo %PYTHON%
  pause
  exit /b 1
)

if not exist "%PYTHONPATH%\fitz" (
  echo [ERRO] PyMuPDF nao encontrado no ambiente local:
  echo %PYTHONPATH%
  pause
  exit /b 1
)

"%PYTHON%" "%SCRIPT%" --pdf "%PDF%" --output "%OUT%"
echo.
echo [Atlas] Concluido.
pause
