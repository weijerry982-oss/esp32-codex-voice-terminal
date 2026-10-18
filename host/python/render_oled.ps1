param(
    [Parameter(Mandatory = $true)][string]$Text,
    [Parameter(Mandatory = $true)][string]$OutPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$clean = ($Text -replace '\s+', ' ').Trim()
if ($clean.Length -eq 0) { $clean = 'OK' }
if ($clean.Length -gt 20) { $clean = $clean.Substring(0, 17) + '...' }

$line1 = $clean
$line2 = ''
if ($clean.Length -gt 10) {
    $line1 = $clean.Substring(0, 10)
    $line2 = $clean.Substring(10)
}

$bmp = New-Object System.Drawing.Bitmap(128, 32)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$font = New-Object System.Drawing.Font('SimSun', 12, [System.Drawing.GraphicsUnit]::Pixel)
$brush = [System.Drawing.Brushes]::Black

if ($line2.Length -eq 0) {
    $g.DrawString($line1, $font, $brush, 4, 9)
} else {
    $g.DrawString($line1, $font, $brush, 4, 2)
    $g.DrawString($line2, $font, $brush, 4, 17)
}
$g.Dispose()

$bytes = New-Object byte[] 512
for ($page = 0; $page -lt 4; $page++) {
    for ($x = 0; $x -lt 128; $x++) {
        $value = 0
        for ($bit = 0; $bit -lt 8; $bit++) {
            $y = $page * 8 + $bit
            $pixel = $bmp.GetPixel($x, $y)
            if ($pixel.R -lt 128) {
                $value = $value -bor (1 -shl $bit)
            }
        }
        $bytes[$page * 128 + $x] = [byte]$value
    }
}
$bmp.Dispose()
[System.IO.File]::WriteAllBytes($OutPath, $bytes)
Write-Output 'OK'
