param(
    [string]$FontPath = "$env:WINDIR\Fonts\NotoSansSC-VF.ttf"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $FontPath)) {
    throw "Font file not found: $FontPath"
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$fontToolsPython = Join-Path $projectRoot ".venv-asr-fixed\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $fontToolsPython)) {
    throw "FontTools Python not found at $fontToolsPython. Install tools\requirements-font.txt into .venv-asr-fixed first."
}

$boldFontPath = Join-Path $env:TEMP "voice_assistant_NotoSansSC_wght600.ttf"
& $fontToolsPython (Join-Path $PSScriptRoot "instantiate_variable_font.py") `
    $FontPath $boldFontPath --weight 600
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create the Noto Sans SC semibold font (exit code $LASTEXITCODE)"
}

$encoding = [System.Text.Encoding]::GetEncoding(936)
$rows = [System.Collections.Generic.List[int]]::new()
for ($row = 0xB0; $row -le 0xF7; $row++) { $rows.Add($row) }

$codepoints = [System.Collections.Generic.SortedSet[int]]::new()
foreach ($row in $rows) {
    for ($column = 0xA1; $column -le 0xFE; $column++) {
        $character = $encoding.GetString([byte[]]@($row, $column))
        if ($character.Length -eq 1 -and $character[0] -ne [char]0xFFFD -and
            [int]$character[0] -notin 0xE810..0xE814) {
            [void]$codepoints.Add([int][char]$character[0])
        }
    }
}

$builder = [System.Text.StringBuilder]::new()
foreach ($codepoint in $codepoints) {
    [void]$builder.Append([char]$codepoint)
}
$symbols = $builder.ToString() + [char]0x22EF

$fontDirectory = Join-Path $projectRoot "main\fonts"
New-Item -ItemType Directory -Force -Path $fontDirectory | Out-Null
$outputPath = Join-Path $fontDirectory "lv_font_chinese_16.c"

npx.cmd --yes lv_font_conv --size 16 --bpp 2 --font $boldFontPath `
    --range "0x20-0x7e" `
    --range "0x00A0-0x00FF" `
    --range "0x2000-0x206F" `
    --range "0x2100-0x214F" `
    --range "0x2190-0x21FF" `
    --range "0x2200-0x22FF" `
    --range "0x2460-0x24FF" `
    --range "0x2500-0x257F" `
    --range "0x25A0-0x25FF" `
    --range "0x3000-0x303F" `
    --range "0xFE30-0xFE4F" `
    --range "0xFF00-0xFFEF" `
    --symbols $symbols `
    --format lvgl --lv-include lvgl.h --lv-font-name lv_font_chinese_16 -o $outputPath
if ($LASTEXITCODE -ne 0) {
    throw "lv_font_conv failed with exit code $LASTEXITCODE"
}

$fontSource = [System.IO.File]::ReadAllText($outputPath)
$fontSource = [System.Text.RegularExpressions.Regex]::Replace(
    $fontSource, '\A/\*.*?\*/\s*', '', [System.Text.RegularExpressions.RegexOptions]::Singleline)
[System.IO.File]::WriteAllText($outputPath, $fontSource, [System.Text.UTF8Encoding]::new($false))

$file = Get-Item -LiteralPath $outputPath
Write-Output "Generated $($codepoints.Count) GB2312 level 1/2 characters, expanded punctuation/symbol ranges, and ASCII at Noto Sans SC weight 600: $($file.Length) bytes"
