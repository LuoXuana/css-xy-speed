$ErrorActionPreference = 'Stop'
try {
    $gameProcesses = @(Get-Process -Name cstrike_win64 -ErrorAction SilentlyContinue)
    if ($gameProcesses.Count -ne 1) { throw 'Start one 64-bit CS:S client and enter a map first.' }
    $knownModules = @('CSSXYSpeed.dll')
    $existingModules = @($gameProcesses[0].Modules | Where-Object { $knownModules -contains $_.ModuleName })
    if ($existingModules.Count -gt 0) {
        throw 'XY Speed is already loaded. Return to the game and press F11 to unload it before restarting.'
    }
    $hudLog = Join-Path $PSScriptRoot 'xy-speed.log'
    $previousText = if (Test-Path -LiteralPath $hudLog) { [IO.File]::ReadAllText($hudLog) } else { '' }
    & (Join-Path $PSScriptRoot 'xy_speed_loader.exe') $gameProcesses[0].Id (Join-Path $PSScriptRoot 'CSSXYSpeed.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Loader failed. See the message above.' }
    $hudReady = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 250
        $newText = ''
        try {
            $fullText = [IO.File]::ReadAllText($hudLog)
            if ($fullText.Length -ge $previousText.Length) { $newText = $fullText.Substring($previousText.Length) }
        } catch { continue }
        if ($newText -match 'FAILED:') { throw ('HUD initialization failed. Check xy-speed.log.' + "`n" + $newText) }
        if ($newText -match 'READY;') { $hudReady = $true; break }
    }
    if (-not $hudReady) { throw 'HUD readiness not confirmed. Check xy-speed.log before retrying.' }
    Write-Host 'XY Speed is ready. Return to CS:S.' -ForegroundColor Green
    Write-Host 'F8: drag position | Shift+F8: show/hide | F9: reload INI | F11: unload'
    Write-Host 'Only horizontal speed is displayed. Your controls and sensitivity are unchanged.'
    exit 0
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
