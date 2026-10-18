param(
    [Parameter(Mandatory = $true)][string]$Text,
    [Parameter(Mandatory = $true)][string]$OutPath,
    [int]$PageChars = 20
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$clean = ($Text -replace '\s+', ' ').Trim()
if ($clean.Length -eq 0) { $clean = 'OK' }
if ($clean.Length -gt 400) { $clean = $clean.Substring(0, 400) }

$pages = @()
for ($offset = 0; $offset -lt $clean.Length; $offset += $PageChars) {
    $length = [Math]::Min($PageChars, $clean.Length - $offset)
    $pages += $clean.Substring($offset, $length)
}
if ($pages.Count -eq 0) { $pages += 'OK' }

$bitmapBytes = New-Object byte[] ($pages.Count * 512)
$font = New-Object System.Drawing.Font('SimSun', 12, [System.Drawing.GraphicsUnit]::Pixel)
$brush = [System.Drawing.Brushes]::Black

for ($pageIndex = 0; $pageIndex -lt $pages.Count; $pageIndex++) {
    $page = $pages[$pageIndex]
    $line1 = $page
    $line2 = ''
    if ($page.Length -gt 10) {
        $line1 = $page.Substring(0, 10)
        $line2 = $page.Substring(10)
    }

    $bmp = New-Object System.Drawing.Bitmap(128, 32)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    if ($line2.Length -eq 0) {
        $g.DrawString($line1, $font, $brush, 4, 9)
    } else {
        $g.DrawString($line1, $font, $brush, 4, 2)
        $g.DrawString($line2, $font, $brush, 4, 17)
    }
    $g.Dispose()

    for ($pageNumber = 0; $pageNumber -lt 4; $pageNumber++) {
        for ($x = 0; $x -lt 128; $x++) {
            $value = 0
            for ($bit = 0; $bit -lt 8; $bit++) {
                $y = $pageNumber * 8 + $bit
                $pixel = $bmp.GetPixel($x, $y)
                if ($pixel.R -lt 128) {
                    $value = $value -bor (1 -shl $bit)
                }
            }
            $bitmapBytes[($pageIndex * 512) + ($pageNumber * 128) + $x] = [byte]$value
        }
    }
    $bmp.Dispose()
}

[System.IO.File]::WriteAllBytes($OutPath, $bitmapBytes)
$font.Dispose()
Write-Output $pages.Count
