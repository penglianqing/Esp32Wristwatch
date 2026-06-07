param(
    [string]$SourcePath = "main/assets/photo_fallback_source.jpg",
    [string]$OutputPath = "main/assets/photo_fallback.rgb565",
    [int]$Size = 466
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile($SourcePath)
try {
    $srcW = [double]$src.Width
    $srcH = [double]$src.Height
    $scale = [Math]::Max($Size / $srcW, $Size / $srcH)
    $drawW = [int][Math]::Ceiling($srcW * $scale)
    $drawH = [int][Math]::Ceiling($srcH * $scale)
    $offsetX = [int][Math]::Floor(($Size - $drawW) / 2)
    $offsetY = [int][Math]::Floor(($Size - $drawH) / 2)

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    try {
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Black)
            $g.DrawImage($src, $offsetX, $offsetY, $drawW, $drawH)
        }
        finally {
            $g.Dispose()
        }

        $bytes = New-Object byte[] ($Size * $Size * 2)
        $pos = 0
        for($y = 0; $y -lt $Size; $y++) {
            for($x = 0; $x -lt $Size; $x++) {
                $c = $bmp.GetPixel($x, $y)
                $value = (($c.R -band 0xF8) -shl 8) -bor (($c.G -band 0xFC) -shl 3) -bor ($c.B -shr 3)
                $bytes[$pos] = [byte]($value -band 0xFF)
                $bytes[$pos + 1] = [byte](($value -shr 8) -band 0xFF)
                $pos += 2
            }
        }

        [System.IO.File]::WriteAllBytes($OutputPath, $bytes)
    }
    finally {
        $bmp.Dispose()
    }
}
finally {
    $src.Dispose()
}
