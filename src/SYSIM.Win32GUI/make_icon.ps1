Add-Type -AssemblyName System.Drawing

# ---------- отрисовка одного размера ----------
function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $f = [float]$s
    $rect = New-Object System.Drawing.RectangleF(0, 0, $f, $f)

    # --- скруглённая плитка (фон) ---
    $r = [float]($s * 0.24)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, 2*$r, 2*$r, 180, 90)
    $path.AddArc($f - 2*$r, 0, 2*$r, 2*$r, 270, 90)
    $path.AddArc($f - 2*$r, $f - 2*$r, 2*$r, 2*$r, 0, 90)
    $path.AddArc(0, $f - 2*$r, 2*$r, 2*$r, 90, 90)
    $path.CloseFigure()

    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 43, 43, 48),    # #2B2B30
        [System.Drawing.Color]::FromArgb(255, 24, 24, 27),    # #18181B
        135)
    $g.FillPath($bg, $path)

    # --- лёгкий верхний блик ---
    $g.SetClip($path)
    $gloss = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.RectangleF(0, 0, $f, $f * 0.55)),
        [System.Drawing.Color]::FromArgb(38, 255, 255, 255),
        [System.Drawing.Color]::FromArgb(0, 255, 255, 255),
        90)
    $g.FillRectangle($gloss, 0, 0, $f, $f * 0.55)
    $g.ResetClip()

    # --- щит с градиентом ---
    $cx = $f / 2
    $shield = New-Object System.Drawing.Drawing2D.GraphicsPath
    $shield.AddLine($cx, $f*0.15, $cx + $f*0.30, $f*0.25)
    $shield.AddLine($cx + $f*0.30, $f*0.25, $cx + $f*0.30, $f*0.50)
    $shield.AddBezier($cx + $f*0.30, $f*0.50, $cx + $f*0.28, $f*0.68, $cx + $f*0.15, $f*0.79, $cx, $f*0.86)
    $shield.AddBezier($cx, $f*0.86, $cx - $f*0.15, $f*0.79, $cx - $f*0.28, $f*0.68, $cx - $f*0.30, $f*0.50)
    $shield.AddLine($cx - $f*0.30, $f*0.50, $cx - $f*0.30, $f*0.25)
    $shield.CloseFigure()

    $shieldBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 76, 194, 255),  # #4CC2FF
        [System.Drawing.Color]::FromArgb(255, 0, 120, 212),  # #0078D4
        90)
    $g.FillPath($shieldBrush, $shield)

    # --- белая «пульс»-линия внутри щита ---
    $pen = New-Object System.Drawing.Pen(
        [System.Drawing.Color]::FromArgb(255, 255, 255, 255), [float]($s * 0.07))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pts = @(
        [System.Drawing.PointF]::new($cx - $f*0.19, $f*0.48),
        [System.Drawing.PointF]::new($cx - $f*0.09, $f*0.48),
        [System.Drawing.PointF]::new($cx - $f*0.04, $f*0.38),
        [System.Drawing.PointF]::new($cx + $f*0.04, $f*0.58),
        [System.Drawing.PointF]::new($cx + $f*0.09, $f*0.48),
        [System.Drawing.PointF]::new($cx + $f*0.19, $f*0.48))
    $g.DrawLines($pen, $pts)

    $g.Dispose()
    return $bmp
}

# ---------- сборка многоразмерной ICO ----------
$sizes = @(16, 24, 32, 48, 64, 256)
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $m = New-Object System.IO.MemoryStream
    $bmp.Save($m, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,($m.ToArray())
    $m.Dispose(); $bmp.Dispose()
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([UInt16]0)                 # reserved
$bw.Write([UInt16]1)                 # тип: иконка
$bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = [byte]0; if ($s -lt 256) { $b = [byte]$s }
    $bw.Write($b); $bw.Write($b)     # ширина/высота (0 = 256)
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length)
    $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
for ($i = 0; $i -lt $sizes.Count; $i++) { $bw.Write($pngs[$i]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes("SYSIM.ico", $ms.ToArray())
$bw.Dispose(); $ms.Dispose()
Write-Host "SYSIM.ico создан: 16/24/32/48/64/256, современный дизайн"