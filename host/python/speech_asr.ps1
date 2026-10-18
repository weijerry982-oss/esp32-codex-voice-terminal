param(
    [Parameter(Mandatory = $true)][string]$WavPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech

$culture = [System.Globalization.CultureInfo]::GetCultureInfo('zh-CN')
$engine = New-Object System.Speech.Recognition.SpeechRecognitionEngine($culture)
$engine.LoadGrammar((New-Object System.Speech.Recognition.DictationGrammar))
$engine.SetInputToWaveFile($WavPath)
$result = $engine.Recognize()
$engine.Dispose()

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
if ($null -ne $result) {
    Write-Output $result.Text
}
