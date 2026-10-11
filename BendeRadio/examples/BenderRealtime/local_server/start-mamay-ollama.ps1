$ErrorActionPreference = 'Stop'
$modelStore = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\artifacts\ollama_mamay_v2'))
$modelManifest = Join-Path $modelStore 'manifests\hf.co\INSAIT-Institute\MamayLM-Gemma-3-12B-IT-v2.0-GGUF\Q4_K_M'
if (-not (Test-Path -LiteralPath $modelManifest)) {
    Write-Error "MamayLM model not found in $modelStore"
    exit 1
}

$ollama = 'C:\Users\Admin\AppData\Local\Programs\Ollama\ollama.exe'
if (-not (Test-Path -LiteralPath $ollama)) {
    $ollama = (Get-Command ollama.exe -ErrorAction SilentlyContinue).Source
}
if (-not $ollama) {
    Write-Error 'ollama.exe not found'
    exit 1
}

$baseUrl = 'http://127.0.0.1:11435'
try {
    $null = Invoke-RestMethod "$baseUrl/api/version" -TimeoutSec 2
} catch {
    $env:OLLAMA_HOST = '127.0.0.1:11435'
    $env:OLLAMA_MODELS = $modelStore
    $env:OLLAMA_NUM_PARALLEL = '1'
    $logDir = Join-Path $modelStore 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $null = Start-Process -FilePath $ollama -ArgumentList 'serve' -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $logDir 'ollama.out.log') `
        -RedirectStandardError (Join-Path $logDir 'ollama.err.log')
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        Start-Sleep -Seconds 1
        try {
            $null = Invoke-RestMethod "$baseUrl/api/version" -TimeoutSec 2
            $ready = $true
            break
        } catch {}
    }
    if (-not $ready) {
        Write-Error 'MamayLM Ollama did not open port 11435'
        exit 1
    }
}

$tags = Invoke-RestMethod "$baseUrl/api/tags" -TimeoutSec 5
$modelName = 'hf.co/INSAIT-Institute/MamayLM-Gemma-3-12B-IT-v2.0-GGUF:Q4_K_M'
if (-not ($tags.models | Where-Object { $_.name -eq $modelName })) {
    Write-Error "Ollama on port 11435 does not have $modelName"
    exit 1
}
Write-Host "MamayLM Ollama ready on $baseUrl"
