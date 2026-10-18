$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$p = Get-Process ChatGPT -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $p) { Write-Output 'ERROR:CHATGPT_NOT_RUNNING'; exit 1 }
$win = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
$all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)

$composer = $null
foreach ($el in $all) {
    try {
        if ($el.Current.ControlType.ProgrammaticName -eq 'ControlType.Edit' -and $el.Current.ClassName -match 'ProseMirror') {
            $composer = $el
        }
    } catch {
    }
}
if ($null -eq $composer) { Write-Output 'ERROR:COMPOSER_NOT_FOUND'; exit 1 }

$vp = $composer.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
$vp.SetValue('')
Start-Sleep -Milliseconds 300
Write-Output 'OK'
