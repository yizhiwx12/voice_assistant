param(
    [string]$Config,
    [string]$Secrets,
    [string]$Profile,
    [string]$Asr,
    [string]$Llm,
    [string]$Tts,
    [ValidateSet("http", "realtime")][string]$Mode = "http",
    [string]$ListenAddress,
    [ValidateRange(1, 65535)][int]$Port = 8000,
    [switch]$ListOptions,
    [switch]$CheckConfig,
    # 进入中文配置菜单；只编辑配置，不启动服务。
    [switch]$Configure
)

$ErrorActionPreference = "Stop"
$serverDirectory = $PSScriptRoot
$projectRoot = Split-Path -Parent $serverDirectory
$python = Join-Path $projectRoot ".venv-voice\Scripts\python.exe"
$server = Join-Path $serverDirectory "voice_server.py"

if (-not (Test-Path -LiteralPath $python)) {
    $python = Join-Path $projectRoot ".venv-asr-fixed\Scripts\python.exe"
}
if (-not (Test-Path -LiteralPath $python)) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if (-not $pythonCommand) {
        throw "Python not found. Install Python or create the project environment at $python"
    }
    $python = $pythonCommand.Source
}
if (-not (Test-Path -LiteralPath $server)) {
    throw "Server script not found: $server"
}

if ($Configure) {
    if ($Asr -or $Llm -or $Tts -or $ListOptions -or $CheckConfig) {
        throw '-Configure opens the configuration menu; use it without -Asr/-Llm/-Tts/-ListOptions/-CheckConfig.'
    }
    $configureArguments = @()
    foreach ($option in @(@('Config', '--config'), @('Secrets', '--secrets'), @('Profile', '--profile'), @('Mode', '--mode'))) {
        if ($PSBoundParameters.ContainsKey($option[0])) {
            $configureArguments += @($option[1], [string]$PSBoundParameters[$option[0]])
        }
    }
    & $python (Join-Path $serverDirectory 'configure_voice.py') @configureArguments
    exit $LASTEXITCODE
}

# 启动参数只设置服务默认组合；设备显式请求 profile 时使用 JSON 中该组合。
$serverArguments = @("--port", "$Port", "--mode", $Mode)
foreach ($option in @(
    @("Config", "--config"), @("Secrets", "--secrets"),
    @("Profile", "--profile"), @("Asr", "--asr"),
    @("Llm", "--llm"), @("Tts", "--tts"), @("ListenAddress", "--host")
)) {
    if ($PSBoundParameters.ContainsKey($option[0])) {
        $serverArguments += @($option[1], [string]$PSBoundParameters[$option[0]])
    }
}
if ($ListOptions) { $serverArguments += "--list-options" }
if ($CheckConfig) { $serverArguments += "--check-config" }

& $python $server @serverArguments
exit $LASTEXITCODE
