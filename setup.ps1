# DLSS5 Converter -- dependency installer.
# Fetches dxgi.dll, the neural add-on, its weights, and the ReShade/add-on config files from
# MediaFire and drops them next to this script (i.e. next to DLSS5ConverterGUI.exe). ffmpeg is
# not part of this -- the exe fetches that itself on first run if it's not already on PATH.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

$logPath = Join-Path $scriptDir 'dlss5-installer.log'
$log = New-Object System.Collections.Generic.List[string]
function Say([string]$text, [string]$color = 'Gray') {
    Write-Host $text -ForegroundColor $color
    $log.Add($text)
}
function Rule([string]$title) {
    $bar = '-' * (60 - $title.Length - 2)
    Say ""
    Say "-- $title $bar" DarkGreen
}

# --- 5x7 block banner, built from a tiny bitmap font instead of hand-typed art so every glyph
# stays aligned by construction ------------------------------------------------------------------
$glyphs = @{
    'D' = @('1111.','1...1','1...1','1...1','1...1','1...1','1111.')
    'L' = @('1....','1....','1....','1....','1....','1....','11111')
    'S' = @('.1111','1....','1....','.111.','....1','....1','1111.')
    '5' = @('11111','1....','1....','1111.','....1','....1','1111.')
    '-' = @('.....','.....','.....','11111','.....','.....','.....')
    'A' = @('.111.','1...1','1...1','11111','1...1','1...1','1...1')
    'M' = @('1...1','11.11','1.1.1','1...1','1...1','1...1','1...1')
}
function Banner([string]$word) {
    # Single-cell blocks (not double-wide) so a longer name like "DLSS5-AMD" still fits an
    # 80-column console without wrapping -- plain ASCII '#' rather than a Unicode block char
    # since the latter mojibakes on a stock Command Prompt whose code page isn't UTF-8.
    # Letters alternate red/green (red for DLSS, a shade of green as the AMD-ish accent).
    $colors = @('Red', 'DarkGreen')
    for ($row = 0; $row -lt 7; $row++) {
        Write-Host "  " -NoNewline
        for ($i = 0; $i -lt $word.Length; $i++) {
            $cell = ($glyphs[[string]$word[$i]][$row] -replace '1','#' -replace '\.',' ')
            Write-Host $cell -NoNewline -ForegroundColor $colors[$i % $colors.Length]
            if ($i -lt $word.Length - 1) { Write-Host ' ' -NoNewline }
        }
        Write-Host ''
    }
}

Clear-Host
Banner 'DLSS5-AMD'
Write-Host ""
Write-Host "   C O N V E R T E R   -   D E P E N D E N C Y   S E T U P" -ForegroundColor DarkGreen
Write-Host ""
Write-Host "[ctrl+c cancel any time]" -NoNewline -ForegroundColor DarkGray
Write-Host "                                            dlss5-image-converter" -ForegroundColor DarkGray

Rule 'What this installs'
Say "  dxgi.dll                    -- ReShade's swap-chain proxy"
Say "  dlss5-neural.addon64        -- the neural upscaling add-on"
Say "  dlssnr_amd_pass1.dll        -- pass-1 runtime"
Say "  dlssnr_on_amd_weights.bin   -- model weights (the big one, ~148 MB)"
Say "  dlssnr_on_amd.ini, ReShade.ini -- config"
Say "  from: https://www.mediafire.com/file/c1amllc1rux130g/AMD-DLSS5-Image-Converter.zip/file"

Rule 'Output'
$installOk = $false
try {
    Say "Locating the download link on MediaFire..."
    $page = Invoke-WebRequest -Uri 'https://www.mediafire.com/file/c1amllc1rux130g/AMD-DLSS5-Image-Converter.zip/file' `
                               -UseBasicParsing -Headers @{ 'User-Agent' = 'Mozilla/5.0' }
    $m = [regex]::Match($page.Content, 'https://download\d+\.mediafire\.com/\S+?\.zip')
    if (-not $m.Success) {
        throw 'Could not find a download link on the MediaFire page -- the page layout may have changed, or the link may be dead.'
    }

    Say "Downloading dependency package (about 105 MB)..."
    Invoke-WebRequest -Uri $m.Value -OutFile 'dlss5convert_deps.zip' -Headers @{ 'User-Agent' = 'Mozilla/5.0' }

    Say "Extracting..."
    Remove-Item -Recurse -Force 'dlss5convert_deps_tmp' -ErrorAction SilentlyContinue
    Expand-Archive -Path 'dlss5convert_deps.zip' -DestinationPath 'dlss5convert_deps_tmp' -Force
    Get-ChildItem 'dlss5convert_deps_tmp' -File | ForEach-Object {
        Copy-Item $_.FullName -Destination $_.Name -Force
        Say "  placed $($_.Name)"
    }
    Remove-Item -Recurse -Force 'dlss5convert_deps_tmp'
    Remove-Item 'dlss5convert_deps.zip'
    $installOk = $true
} catch {
    Say "FAIL $($_.Exception.Message)" Red
}

Rule ''
if ($installOk) {
    Write-Host "Install SUCCEEDED." -ForegroundColor Green -NoNewline
    Write-Host "  You can now run DLSS5ConverterGUI.exe." -ForegroundColor Gray
    Remove-Item $logPath -ErrorAction SilentlyContinue
} else {
    Write-Host "Install FAILED." -ForegroundColor Red -NoNewline
    $log | Set-Content -Path $logPath -Encoding utf8
    Write-Host "  Log written to $logPath" -ForegroundColor Gray
    Write-Host "You can also grab the zip by hand from the MediaFire link above and drop its" -ForegroundColor Gray
    Write-Host "contents next to DLSS5ConverterGUI.exe yourself." -ForegroundColor Gray
}
Write-Host ""

if (-not $installOk) { exit 1 }
