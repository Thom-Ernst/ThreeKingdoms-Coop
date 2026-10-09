# Deterministic display-size export. Never writes to assets/ (the preserved sources).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$destination = Join-Path $PSScriptRoot 'build/ui/skins/coop'
$null = New-Item -ItemType Directory -Path $destination -Force
foreach ($number in 1..3) {
    $source = Join-Path $PSScriptRoot "assets/extracted/gift_recipient_$number.png"
    $inputImage = [System.Drawing.Bitmap]::FromFile($source)
    $outputImage = New-Object System.Drawing.Bitmap(46,46,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($outputImage)
    try {
        if ($inputImage.GetPixel(0,0).A -ne 0) { throw "Source must have transparent margins: $source" }
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($inputImage, [System.Drawing.Rectangle]::new(0,0,46,46), 0,0,$inputImage.Width,$inputImage.Height,[System.Drawing.GraphicsUnit]::Pixel)
        $outputImage.Save((Join-Path $destination "gift_recipient_$number.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $outputImage.Dispose()
        $inputImage.Dispose()
    }
}
Write-Output 'Exported three 46x46 textures; original and extracted sources untouched.'
