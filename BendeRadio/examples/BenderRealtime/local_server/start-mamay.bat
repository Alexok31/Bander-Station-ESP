@echo off
setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-mamay-ollama.ps1"
if errorlevel 1 (
  echo MamayLM Ollama could not start. Existing Bender server was not changed.
  pause
  exit /b 1
)

set "BENDER_LLM=local"
set "OLLAMA_URL=http://127.0.0.1:11435"
set "OLLAMA_MODEL=hf.co/INSAIT-Institute/MamayLM-Gemma-3-12B-IT-v2.0-GGUF:Q4_K_M"
set "OLLAMA_NUM_GPU=36"
set "OLLAMA_NUM_CTX=4096"
set "OLLAMA_HISTORY_MESSAGES=8"
set "OLLAMA_SUMMARY_CHARS=600"
set "OLLAMA_COMPACT_PROMPT=1"
call "%~dp0start.bat" %*
