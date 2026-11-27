# Build / flash / smoke-test helper. Prints only the essentials (keeps logs short).
#   dev.ps1 [all|build|ota|check|usb] [-Ip 192.168.50.62] [-Pass <panel password>]
param(
  [ValidateSet('all', 'build', 'ota', 'check', 'usb')][string]$Cmd = 'all',
  [string]$Ip = '192.168.50.62',
  [string]$Pass = $env:P5_PASS
)
$ErrorActionPreference = 'Continue'   # native tools write progress to stderr; exit codes are checked instead
$root = Split-Path $PSScriptRoot -Parent
$pio  = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
# esptool prints Unicode progress bars; in a cp1251 console PIO's output thread crashes and the upload hangs
$env:PYTHONUTF8 = '1'; $env:PYTHONIOENCODING = 'utf-8'
$bin  = "$root\.pio\build\esp32-s3-zero\firmware.bin"
$auth = if ($Pass) { @('-u', "admin:$Pass") } else { @() }

function Invoke-Build {
  $log = & $pio run -d $root 2>&1 | Out-String
  $keep = $log -split "`r?`n" | Where-Object { $_ -match 'error|warning: unused|RAM:|Flash:|SUCCESS|FAILED' }
  $keep | Select-Object -First 40 | ForEach-Object { $_.Trim() }
  if ($LASTEXITCODE -ne 0) { Write-Output 'BUILD FAILED'; exit 1 }
}

function Wait-Board {
  for ($i = 0; $i -lt 30; $i++) {
    $code = & curl.exe -s -o NUL -w '%{http_code}' -m 2 @auth "http://$Ip/"
    if ($code -eq '200') { return $true }
    Start-Sleep -Seconds 1
  }
  return $false
}

function Invoke-Ota {
  if (-not (Test-Path $bin)) { Write-Output "no $bin - build first"; exit 1 }
  $t = Get-Date
  $resp = & curl.exe -s -m 120 @auth -F "fw=@$bin" "http://$Ip/update" | Out-String
  if ($resp -notmatch "url=/'") {               # only the restart page has this redirect
    $txt = ($resp -replace '<style[\s\S]*?</style>', '' -replace '<[^>]+>', ' ' -replace '\s+', ' ').Trim()
    Write-Output "OTA FAILED: $($txt.Substring(0, [Math]::Min(300, $txt.Length)))"; exit 1
  }
  Start-Sleep -Seconds 3
  if (-not (Wait-Board)) { Write-Output 'OTA sent, board did not come back in 30 s'; exit 1 }
  Write-Output ("OTA OK in {0:N0} s" -f ((Get-Date) - $t).TotalSeconds)
}

function Invoke-Check {
  $bad = 0
  foreach ($p in '/', '/dns', '/bypass', '/config', '/style.css', '/app.js') {
    $code = & curl.exe -s -o NUL -w '%{http_code}' -m 5 @auth "http://$Ip$p"
    if ($code -ne '200') { Write-Output "FAIL $p -> $code"; $bad++ }
  }
  # Live values API: valid JSON with the uptime key
  try { $j = (& curl.exe -s -m 5 @auth "http://$Ip/api/status" | Out-String) | ConvertFrom-Json } catch { $j = $null }
  if (-not $j -or -not $j.up) { Write-Output 'FAIL /api/status: no JSON'; $bad++ }
  # Chart history API: both sets are valid JSON with the interval key
  foreach ($set in 'sys', 'dns') {
    try { $h = (& curl.exe -s -m 5 @auth "http://$Ip/api/hist?set=$set" | Out-String) | ConvertFrom-Json } catch { $h = $null }
    if (-not $h -or -not $h.iv) { Write-Output "FAIL /api/hist?set=${set}: no JSON"; $bad++ }
  }
  # Static files are stored and sent gzipped
  $hd = & curl.exe -s -o NUL -D - -m 5 -H 'Accept-Encoding: gzip' "http://$Ip/style.css" | Out-String
  if ($hd -notmatch 'Content-Encoding: gzip') { Write-Output 'FAIL /style.css: not gzip'; $bad++ }
  # cmd /c: nslookup writes "Non-authoritative answer" to stderr, which PowerShell 5.1 treats as an error
  $ns = cmd /c "nslookup doubleclick.net $Ip 2>nul" | Out-String
  if ($ns -notmatch '0\.0\.0\.0') {
    # The on/off tag sits between the first toggle row and its /dns/toggle button; "bad" = switched off
    $html = & curl.exe -s -m 5 @auth "http://$Ip/dns" | Out-String
    $a = $html.IndexOf('<div class=row><b>'); $b = $html.IndexOf('/dns/toggle')
    $off = $a -ge 0 -and $b -gt $a -and $html.Substring($a, $b - $a) -match 'bad'
    if ($off) { Write-Output 'WARN DNS: filter is switched OFF in the panel - blocking not tested' }
    else { Write-Output 'FAIL DNS: doubleclick.net not blocked'; $bad++ }
  }
  # nslookup prints the server address first, so a resolved name gives at least 2 IPv4 addresses
  $ok = cmd /c "nslookup example.com $Ip 2>nul" | Out-String
  $n = ([regex]::Matches($ok, '\b\d+\.\d+\.\d+\.\d+\b')).Count
  if ($n -lt 2 -or $ok -match '0\.0\.0\.0') { Write-Output 'FAIL DNS: example.com not resolved'; $bad++ }
  if ($bad) { exit 1 } else { Write-Output 'CHECK OK (6 pages 200, api + hist JSON, gzip css, DNS blocks + resolves)' }
}

switch ($Cmd) {
  'build' { Invoke-Build }
  'ota'   { Invoke-Ota }
  'check' { Invoke-Check }
  'usb'   { & $pio run -d $root -t upload 2>&1 | Select-String 'error|SUCCESS|FAILED|Hard resetting' | ForEach-Object { $_.Line.Trim() } }
  'all'   { Invoke-Build; Invoke-Ota; Invoke-Check }
}
