param(
    [Parameter(Mandatory = $true)][string]$Draft,
    [int]$TimeoutSec = 240
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$p = Get-Process ChatGPT -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output 'ERROR:CHATGPT_NOT_RUNNING'; exit 1 }
$win = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)

function Get-All($root) {
    return $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
}

$warningNeedle = [string]([char]0x53EF) + [char]0x80FD + [char]0x4F1A + [char]0x51FA + [char]0x9519
$doneNeedle = [string]([char]0x56DE) + [char]0x7B54 + [char]0x5DF2 + [char]0x5B8C + [char]0x6210
$latestNeedle = [string]([char]0x6700) + [char]0x65B0 + [char]0x4E00 + [char]0x6761 + [char]0x56DE + [char]0x590D
$transientNeedles = @(
    ([string]([char]0x6B63) + [char]0x5728 + [char]0x601D + [char]0x8003),
    ([string]([char]0x6B63) + [char]0x5728 + [char]0x641C + [char]0x7D22),
    ([string]([char]0x641C) + [char]0x7D22 + [char]0x4E2D),
    ([string]([char]0x6B63) + [char]0x5728 + [char]0x751F + [char]0x6210),
    ([string]([char]0x56DE) + [char]0x590D + [char]0x4E2D),
    ([string]([char]0x7B49) + [char]0x5F85 + [char]0x56DE + [char]0x590D),
    ([string]([char]0x505C) + [char]0x6B62 + [char]0x751F + [char]0x6210),
    'Thinking',
    'Searching',
    'Stop generating'
)

$all = Get-All $win
$composer = $null
$send = $null
foreach ($el in $all) {
    try {
        if ($el.Current.ControlType.ProgrammaticName -eq 'ControlType.Edit' -and
            $el.Current.ClassName -match 'ProseMirror') {
            $composer = $el
        }
        if ($el.Current.ControlType.ProgrammaticName -eq 'ControlType.Button' -and
            $el.Current.ClassName -match 'size-token-button-composer') {
            $send = $el
        }
    } catch {
    }
}
if ($null -eq $send -or -not $send.Current.IsEnabled) {
    Write-Output 'ERROR:SEND_NOT_READY'
    exit 1
}
if ($null -ne $composer) {
    $vp = $composer.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $currentDraft = $vp.Current.Value
    if ($null -ne $currentDraft -and $currentDraft.Trim().Length -gt 0) {
        $Draft = $currentDraft
    }
}

$send.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()

$probeLength = [Math]::Min(24, $Draft.Length)
$probe = $Draft.Substring(0, $probeLength)
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$lastText = ''
$stableCount = 0

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $all = Get-All $win

    $userIndex = -1
    for ($i = 0; $i -lt $all.Count; $i++) {
        try {
            if ($all[$i].Current.ControlType.ProgrammaticName -eq 'ControlType.Text' -and
                $all[$i].Current.Name.Contains($probe)) {
                $userIndex = $i
            }
        } catch {
        }
    }
    if ($userIndex -lt 0) { continue }

    $texts = @()
    for ($j = $userIndex + 1; $j -lt $all.Count; $j++) {
        try {
            $ct = $all[$j].Current.ControlType.ProgrammaticName
            if ($ct -eq 'ControlType.Edit') { break }
            if ($ct -eq 'ControlType.Text') {
                $name = $all[$j].Current.Name.Trim()
                if ($name.Length -eq 0) { continue }
                if ($name.Contains($warningNeedle)) { continue }
                if ($name.Contains($doneNeedle)) { continue }
                if ($name.Contains($latestNeedle)) { continue }
                if ($name -match '^\d{1,2}:\d{2}(:\d{2})?$') { continue }
                $isTransient = $false
                foreach ($needle in $transientNeedles) {
                    if ($name -eq $needle -or ($name.Length -le 20 -and $name.Contains($needle))) {
                        $isTransient = $true
                        break
                    }
                }
                if ($isTransient) { continue }
                $texts += $name
            }
        } catch {
        }
    }

    $candidate = ($texts -join "`n").Trim()
    if ($candidate.Length -lt 8) { continue }
    if ($candidate -eq $lastText) {
        $stableCount++
    } else {
        $lastText = $candidate
        $stableCount = 0
    }
    if ($stableCount -ge 2) {
        Write-Output $candidate
        exit 0
    }
}

if ($lastText.Length -gt 0) {
    Write-Output $lastText
} else {
    Write-Output 'ERROR:NO_ANSWER'
}
