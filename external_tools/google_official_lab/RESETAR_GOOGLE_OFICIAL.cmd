@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "TOOL_DIR=%~dp0"
set "OUT=%TOOL_DIR%output"

echo [Atlas] Resetando saidas do Google oficial...
if exist "%OUT%\*_google_oficial_importavel.tsv" del "%OUT%\*_google_oficial_importavel.tsv"
if exist "%OUT%\*_google_oficial_bruto.jsonl" del "%OUT%\*_google_oficial_bruto.jsonl"
if exist "%OUT%\*_google_oficial_progresso.jsonl" del "%OUT%\*_google_oficial_progresso.jsonl"
if exist "%OUT%\*_google_oficial_relatorio.txt" del "%OUT%\*_google_oficial_relatorio.txt"
if exist "%OUT%\*_google_oficial_ignoradas.tsv" del "%OUT%\*_google_oficial_ignoradas.tsv"
if exist "%OUT%\google_oficial_importavel.tsv" del "%OUT%\google_oficial_importavel.tsv"
if exist "%OUT%\google_oficial_bruto.jsonl" del "%OUT%\google_oficial_bruto.jsonl"
if exist "%OUT%\google_oficial_progresso.jsonl" del "%OUT%\google_oficial_progresso.jsonl"
if exist "%OUT%\google_oficial_relatorio.txt" del "%OUT%\google_oficial_relatorio.txt"
if exist "%OUT%\google_oficial_ignoradas.tsv" del "%OUT%\google_oficial_ignoradas.tsv"

echo [Atlas] Reset concluido.
pause
