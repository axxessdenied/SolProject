# Regenerates the procedural source assets (textures: checker.png, hull.png;
# meshes: cube.gltf, station.gltf, ship.gltf, asteroid.gltf). Windows PowerShell 7+,
# System.Drawing. NOTE: parenthesize all arithmetic inside array literals -
# the comma binds tighter than + and silently corrupts data otherwise.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path

# --- checker.png: 256x256, 32px squares, warm orange / deep space blue ---
Add-Type -AssemblyName System.Drawing
$size = 256; $cell = 32
$bmp = New-Object System.Drawing.Bitmap($size, $size)
$colorA = [System.Drawing.Color]::FromArgb(255, 232, 122, 42)   # orange
$colorB = [System.Drawing.Color]::FromArgb(255, 30, 34, 48)     # dark blue-gray
$g = [System.Drawing.Graphics]::FromImage($bmp)
$brushA = New-Object System.Drawing.SolidBrush($colorA)
$brushB = New-Object System.Drawing.SolidBrush($colorB)
for ($y = 0; $y -lt $size / $cell; $y++) {
    for ($x = 0; $x -lt $size / $cell; $x++) {
        $brush = if ((($x + $y) % 2) -eq 0) { $brushA } else { $brushB }
        $g.FillRectangle($brush, $x * $cell, $y * $cell, $cell, $cell)
    }
}
# small marker square so orientation is visible
$g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), 4, 4, 12, 12)
$g.Dispose()
New-Item -ItemType Directory -Force "$repo\assets\textures" | Out-Null
$bmp.Save("$repo\assets\textures\checker.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "wrote checker.png"

# --- cube.gltf: unit cube, 24 verts (per-face normals/uvs), CCW winding ---
$positions = New-Object System.Collections.Generic.List[float]
$normals = New-Object System.Collections.Generic.List[float]
$uvs = New-Object System.Collections.Generic.List[float]
$indices = New-Object System.Collections.Generic.List[uint16]

# Each face: normal, then 4 corners CCW viewed from outside.
$faces = @(
    @{ n = @(0,0,1);  c = @(@(-0.5,-0.5,0.5), @(0.5,-0.5,0.5), @(0.5,0.5,0.5), @(-0.5,0.5,0.5)) },
    @{ n = @(0,0,-1); c = @(@(0.5,-0.5,-0.5), @(-0.5,-0.5,-0.5), @(-0.5,0.5,-0.5), @(0.5,0.5,-0.5)) },
    @{ n = @(1,0,0);  c = @(@(0.5,-0.5,0.5), @(0.5,-0.5,-0.5), @(0.5,0.5,-0.5), @(0.5,0.5,0.5)) },
    @{ n = @(-1,0,0); c = @(@(-0.5,-0.5,-0.5), @(-0.5,-0.5,0.5), @(-0.5,0.5,0.5), @(-0.5,0.5,-0.5)) },
    @{ n = @(0,1,0);  c = @(@(-0.5,0.5,0.5), @(0.5,0.5,0.5), @(0.5,0.5,-0.5), @(-0.5,0.5,-0.5)) },
    @{ n = @(0,-1,0); c = @(@(-0.5,-0.5,-0.5), @(0.5,-0.5,-0.5), @(0.5,-0.5,0.5), @(-0.5,-0.5,0.5)) }
)
$faceUvs = @(@(0,1), @(1,1), @(1,0), @(0,0))

$base = 0
foreach ($face in $faces) {
    for ($i = 0; $i -lt 4; $i++) {
        $positions.AddRange([float[]]$face.c[$i])
        $normals.AddRange([float[]]$face.n)
        $uvs.AddRange([float[]]$faceUvs[$i])
    }
    $indices.AddRange([uint16[]]@($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3)))
    $base += 4
}

$bytes = New-Object System.Collections.Generic.List[byte]
foreach ($f in $positions) { $bytes.AddRange([BitConverter]::GetBytes($f)) }
$normalsOffset = $bytes.Count
foreach ($f in $normals) { $bytes.AddRange([BitConverter]::GetBytes($f)) }
$uvsOffset = $bytes.Count
foreach ($f in $uvs) { $bytes.AddRange([BitConverter]::GetBytes($f)) }
$indicesOffset = $bytes.Count
foreach ($i in $indices) { $bytes.AddRange([BitConverter]::GetBytes($i)) }
$total = $bytes.Count
$b64 = [Convert]::ToBase64String($bytes.ToArray())

$gltf = @"
{
  "asset": { "version": "2.0", "generator": "SolProject asset script" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [ { "mesh": 0, "name": "cube" } ],
  "meshes": [ {
    "name": "cube",
    "primitives": [ {
      "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 },
      "indices": 3
    } ]
  } ],
  "buffers": [ { "byteLength": $total, "uri": "data:application/octet-stream;base64,$b64" } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": $normalsOffset },
    { "buffer": 0, "byteOffset": $normalsOffset, "byteLength": $($uvsOffset - $normalsOffset) },
    { "buffer": 0, "byteOffset": $uvsOffset, "byteLength": $($indicesOffset - $uvsOffset) },
    { "buffer": 0, "byteOffset": $indicesOffset, "byteLength": $($total - $indicesOffset) }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
      "min": [-0.5,-0.5,-0.5], "max": [0.5,0.5,0.5] },
    { "bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 36, "type": "SCALAR" }
  ]
}
"@
New-Item -ItemType Directory -Force "$repo\assets\meshes" | Out-Null
[System.IO.File]::WriteAllText("$repo\assets\meshes\cube.gltf", $gltf)
Write-Output "wrote cube.gltf ($total buffer bytes)"

# --- hull.png: 256x256 dark panel grid for station/ship hulls ---
$size = 256
$bmp = New-Object System.Drawing.Bitmap($size, $size)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(255, 96, 100, 108))
$rng = New-Object System.Random(1337)
for ($i = 0; $i -lt 60; $i++) {
    $w = $rng.Next(16, 64); $h = $rng.Next(12, 48)
    $x = $rng.Next(0, $size - $w); $y = $rng.Next(0, $size - $h)
    $shade = $rng.Next(78, 126)
    $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, $shade, ($shade + 3), ($shade + 8)))
    $g.FillRectangle($brush, $x, $y, $w, $h)
    $brush.Dispose()
}
$linePen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 58, 60, 66), 2)
for ($i = 0; $i -lt 8; $i++) {
    $x = $rng.Next(0, $size); $g.DrawLine($linePen, $x, 0, $x, $size)
    $y = $rng.Next(0, $size); $g.DrawLine($linePen, 0, $y, $size, $y)
}
$accent = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 205, 130, 40))
for ($i = 0; $i -lt 6; $i++) {
    $g.FillRectangle($accent, $rng.Next(0, $size - 20), $rng.Next(0, $size - 6), 20, 6)
}
$accent.Dispose(); $linePen.Dispose(); $g.Dispose()
$bmp.Save("$repo\assets\textures\hull.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "wrote hull.png"

# --- shared glTF mesh emit helpers ---
function New-MeshBuilder {
    [pscustomobject]@{
        Positions = New-Object System.Collections.Generic.List[float]
        Normals   = New-Object System.Collections.Generic.List[float]
        Uvs       = New-Object System.Collections.Generic.List[float]
        Indices   = New-Object System.Collections.Generic.List[uint16]
    }
}

function Add-Vertex($mb, $px, $py, $pz, $nx, $ny, $nz, $u, $v) {
    $mb.Positions.AddRange([float[]]@($px, $py, $pz))
    $mb.Normals.AddRange([float[]]@($nx, $ny, $nz))
    $mb.Uvs.AddRange([float[]]@($u, $v))
}

# Axis-aligned box centered at (cx,cy,cz) with full sizes (sx,sy,sz).
function Add-Box($mb, $cx, $cy, $cz, $sx, $sy, $sz) {
    $hx = $sx / 2; $hy = $sy / 2; $hz = $sz / 2
    $boxFaces = @(
        @{ n = @(0,0,1);  c = @(@((-$hx),(-$hy),$hz), @($hx,(-$hy),$hz), @($hx,$hy,$hz), @((-$hx),$hy,$hz)) },
        @{ n = @(0,0,-1); c = @(@($hx,(-$hy),(-$hz)), @((-$hx),(-$hy),(-$hz)), @((-$hx),$hy,(-$hz)), @($hx,$hy,(-$hz))) },
        @{ n = @(1,0,0);  c = @(@($hx,(-$hy),$hz), @($hx,(-$hy),(-$hz)), @($hx,$hy,(-$hz)), @($hx,$hy,$hz)) },
        @{ n = @(-1,0,0); c = @(@((-$hx),(-$hy),(-$hz)), @((-$hx),(-$hy),$hz), @((-$hx),$hy,$hz), @((-$hx),$hy,(-$hz))) },
        @{ n = @(0,1,0);  c = @(@((-$hx),$hy,$hz), @($hx,$hy,$hz), @($hx,$hy,(-$hz)), @((-$hx),$hy,(-$hz))) },
        @{ n = @(0,-1,0); c = @(@((-$hx),(-$hy),(-$hz)), @($hx,(-$hy),(-$hz)), @($hx,(-$hy),$hz), @((-$hx),(-$hy),$hz)) }
    )
    $cornerUvs = @(@(0,1), @(1,1), @(1,0), @(0,0))
    foreach ($face in $boxFaces) {
        $base = $mb.Positions.Count / 3
        for ($i = 0; $i -lt 4; $i++) {
            $p = $face.c[$i]
            Add-Vertex $mb ($p[0] + $cx) ($p[1] + $cy) ($p[2] + $cz) $face.n[0] $face.n[1] $face.n[2] $cornerUvs[$i][0] $cornerUvs[$i][1]
        }
        $mb.Indices.AddRange([uint16[]]@($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3)))
    }
}

# Torus in the XZ plane centered at the origin.
function Add-Torus($mb, $majorRadius, $tubeRadius, $segU, $segV, $uTiles) {
    $base = $mb.Positions.Count / 3
    for ($i = 0; $i -le $segU; $i++) {
        $phi = 2 * [Math]::PI * $i / $segU
        $cp = [Math]::Cos($phi); $sp = [Math]::Sin($phi)
        for ($j = 0; $j -le $segV; $j++) {
            $theta = 2 * [Math]::PI * $j / $segV
            $ct = [Math]::Cos($theta); $st = [Math]::Sin($theta)
            $px = ($majorRadius + ($tubeRadius * $ct)) * $cp
            $py = $tubeRadius * $st
            $pz = ($majorRadius + ($tubeRadius * $ct)) * $sp
            Add-Vertex $mb $px $py $pz ($ct * $cp) $st ($ct * $sp) ($uTiles * $i / $segU) ($j / $segV)
        }
    }
    $stride = $segV + 1
    for ($i = 0; $i -lt $segU; $i++) {
        for ($j = 0; $j -lt $segV; $j++) {
            $a = $base + ($i * $stride) + $j
            $b = $a + $stride
            # CCW viewed from outside the tube.
            $mb.Indices.AddRange([uint16[]]@($a, ($a + 1), ($b + 1), $a, ($b + 1), $b))
        }
    }
}

# Triangle with a flat computed normal (faceted hulls).
function Add-FlatTriangle($mb, $p0, $p1, $p2, $uv0, $uv1, $uv2) {
    $ux = $p1[0] - $p0[0]; $uy = $p1[1] - $p0[1]; $uz = $p1[2] - $p0[2]
    $vx = $p2[0] - $p0[0]; $vy = $p2[1] - $p0[1]; $vz = $p2[2] - $p0[2]
    $nx = ($uy * $vz) - ($uz * $vy)
    $ny = ($uz * $vx) - ($ux * $vz)
    $nz = ($ux * $vy) - ($uy * $vx)
    $len = [Math]::Sqrt(($nx * $nx) + ($ny * $ny) + ($nz * $nz))
    if ($len -gt 0) { $nx /= $len; $ny /= $len; $nz /= $len }
    $base = $mb.Positions.Count / 3
    Add-Vertex $mb $p0[0] $p0[1] $p0[2] $nx $ny $nz $uv0[0] $uv0[1]
    Add-Vertex $mb $p1[0] $p1[1] $p1[2] $nx $ny $nz $uv1[0] $uv1[1]
    Add-Vertex $mb $p2[0] $p2[1] $p2[2] $nx $ny $nz $uv2[0] $uv2[1]
    $mb.Indices.AddRange([uint16[]]@($base, ($base + 1), ($base + 2)))
}

function Write-Gltf($mb, $name, $path) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    foreach ($f in $mb.Positions) { $bytes.AddRange([BitConverter]::GetBytes([float]$f)) }
    $normalsOffset = $bytes.Count
    foreach ($f in $mb.Normals) { $bytes.AddRange([BitConverter]::GetBytes([float]$f)) }
    $uvsOffset = $bytes.Count
    foreach ($f in $mb.Uvs) { $bytes.AddRange([BitConverter]::GetBytes([float]$f)) }
    $indicesOffset = $bytes.Count
    foreach ($i in $mb.Indices) { $bytes.AddRange([BitConverter]::GetBytes([uint16]$i)) }
    $total = $bytes.Count
    $b64 = [Convert]::ToBase64String($bytes.ToArray())
    $vertexCount = $mb.Positions.Count / 3
    $indexCount = $mb.Indices.Count

    $minX = [float]::MaxValue; $minY = [float]::MaxValue; $minZ = [float]::MaxValue
    $maxX = [float]::MinValue; $maxY = [float]::MinValue; $maxZ = [float]::MinValue
    for ($i = 0; $i -lt $mb.Positions.Count; $i += 3) {
        $x = $mb.Positions[$i]; $y = $mb.Positions[($i + 1)]; $z = $mb.Positions[($i + 2)]
        if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
        if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        if ($z -lt $minZ) { $minZ = $z }; if ($z -gt $maxZ) { $maxZ = $z }
    }
    $ci = [System.Globalization.CultureInfo]::InvariantCulture
    $minStr = '{0},{1},{2}' -f $minX.ToString($ci), $minY.ToString($ci), $minZ.ToString($ci)
    $maxStr = '{0},{1},{2}' -f $maxX.ToString($ci), $maxY.ToString($ci), $maxZ.ToString($ci)

    $json = @"
{
  "asset": { "version": "2.0", "generator": "SolProject asset script" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [ { "mesh": 0, "name": "$name" } ],
  "meshes": [ {
    "name": "$name",
    "primitives": [ {
      "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 },
      "indices": 3
    } ]
  } ],
  "buffers": [ { "byteLength": $total, "uri": "data:application/octet-stream;base64,$b64" } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": $normalsOffset },
    { "buffer": 0, "byteOffset": $normalsOffset, "byteLength": $($uvsOffset - $normalsOffset) },
    { "buffer": 0, "byteOffset": $uvsOffset, "byteLength": $($indicesOffset - $uvsOffset) },
    { "buffer": 0, "byteOffset": $indicesOffset, "byteLength": $($total - $indicesOffset) }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": $vertexCount, "type": "VEC3",
      "min": [$minStr], "max": [$maxStr] },
    { "bufferView": 1, "componentType": 5126, "count": $vertexCount, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": $vertexCount, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": $indexCount, "type": "SCALAR" }
  ]
}
"@
    [System.IO.File]::WriteAllText($path, $json)
    Write-Output "wrote $(Split-Path -Leaf $path) ($vertexCount verts, $indexCount indices)"
}

# --- station.gltf: habitat torus + hub + spokes + solar panels (meters) ---
$station = New-MeshBuilder
Add-Torus $station 90 12 40 12 8
Add-Box $station 0 0 0 44 60 44                     # hub
Add-Box $station 47 0 0 86 6 6                      # spokes
Add-Box $station -47 0 0 86 6 6
Add-Box $station 0 0 47 6 6 86
Add-Box $station 0 0 -47 6 6 86
Add-Box $station 0 44 0 3 28 3                      # panel masts
Add-Box $station 0 -44 0 3 28 3
Add-Box $station 0 62 0 76 1.5 26                   # solar panels
Add-Box $station 0 -62 0 76 1.5 26
Write-Gltf $station 'station' "$repo\assets\meshes\station.gltf"

# --- ship.gltf: faceted wedge fighter, nose at -Z (~12 m long) ---
$ship = New-MeshBuilder
$nose = @(0, 0.3, -7)
# front ring (z = -1), rear ring (z = 5): bottom-left, bottom-right, top-right, top-left
$f = @(@(-2.6, -1.3, -1), @(2.6, -1.3, -1), @(2.0, 1.5, -1), @(-2.0, 1.5, -1))
$r = @(@(-3.5, -1.7, 5), @(3.5, -1.7, 5), @(2.6, 2.0, 5), @(-2.6, 2.0, 5))
for ($i = 0; $i -lt 4; $i++) {
    $j = ($i + 1) % 4
    # nose cap fan (CCW seen from outside)
    Add-FlatTriangle $ship $nose $f[$j] $f[$i] @(0.5, 0) @(1, 1) @(0, 1)
    # hull quad between rings
    Add-FlatTriangle $ship $f[$i] $f[$j] $r[$j] @(0, 0) @(1, 0) @(1, 1)
    Add-FlatTriangle $ship $f[$i] $r[$j] $r[$i] @(0, 0) @(1, 1) @(0, 1)
}
# rear cap (CCW seen from +Z, outside): bl -> br -> tr, bl -> tr -> tl
Add-FlatTriangle $ship $r[0] $r[1] $r[2] @(0, 0) @(1, 0) @(1, 1)
Add-FlatTriangle $ship $r[0] $r[2] $r[3] @(0, 0) @(1, 1) @(0, 1)
# dorsal fin
Add-FlatTriangle $ship @(0, 1.7, 1) @(0, 3.8, 4.6) @(0, 2.0, 5) @(0, 0) @(0.5, 1) @(1, 0)
Add-FlatTriangle $ship @(0, 1.7, 1) @(0, 2.0, 5) @(0, 3.8, 4.6) @(0, 0) @(1, 0) @(0.5, 1)
Write-Gltf $ship 'ship' "$repo\assets\meshes\ship.gltf"

# --- asteroid.gltf: noise-displaced icosphere, faceted, unit radius ~1 m ---
# The game scales one mesh per rock (RenderShape scale = the rock's radius in
# meters), so this is authored at radius 1 and every asteroid in the galaxy is
# the same hull at a different size and tumble.
function Normalize3($p) {
    $len = [Math]::Sqrt(($p[0] * $p[0]) + ($p[1] * $p[1]) + ($p[2] * $p[2]))
    if ($len -le 0) { return @(0, 0, 0) }
    return @(($p[0] / $len), ($p[1] / $len), ($p[2] / $len))
}

# Displacement is keyed on the (rounded) unit direction so vertices shared by
# neighboring triangles always agree and the hull stays closed.
$script:rockRadii = @{}
function Get-RockRadius($p) {
    $key = '{0:F4}|{1:F4}|{2:F4}' -f $p[0], $p[1], $p[2]
    if ($script:rockRadii.ContainsKey($key)) { return $script:rockRadii[$key] }
    $h = [uint32]2166136261
    foreach ($ch in $key.ToCharArray()) {
        # 0xFFFFFFFFL, not 0xFFFFFFFF: the unsuffixed literal parses as Int32
        # -1, so the mask would be a no-op and the cast would overflow.
        $h = [uint32]((($h -bxor [uint32][int]$ch) * 16777619) -band 0xFFFFFFFFL)
    }
    $jitter = ($h % 10000) / 10000.0
    # Low-frequency lobes on top of the per-vertex jitter, so the rock reads as
    # a lumpy body rather than a sphere with sandpaper on it.
    $lump = (0.13 * [Math]::Sin((3.1 * $p[0]) + 1.7)) +
            (0.11 * [Math]::Sin((2.6 * $p[1]) + 0.4)) +
            (0.09 * [Math]::Sin((3.7 * $p[2]) + 2.3))
    $radius = 0.80 + (0.17 * $jitter) + $lump
    $script:rockRadii[$key] = $radius
    return $radius
}

$phi = (1 + [Math]::Sqrt(5)) / 2
$icoVerts = @(
    @(-1, $phi, 0), @(1, $phi, 0), @(-1, (-$phi), 0), @(1, (-$phi), 0),
    @(0, -1, $phi), @(0, 1, $phi), @(0, -1, (-$phi)), @(0, 1, (-$phi)),
    @($phi, 0, -1), @($phi, 0, 1), @((-$phi), 0, -1), @((-$phi), 0, 1)
)
$icoFaces = @(
    @(0, 11, 5), @(0, 5, 1), @(0, 1, 7), @(0, 7, 10), @(0, 10, 11),
    @(1, 5, 9), @(5, 11, 4), @(11, 10, 2), @(10, 7, 6), @(7, 1, 8),
    @(3, 9, 4), @(3, 4, 2), @(3, 2, 6), @(3, 6, 8), @(3, 8, 9),
    @(4, 9, 5), @(2, 4, 11), @(6, 2, 10), @(8, 6, 7), @(9, 8, 1)
)

$tris = New-Object System.Collections.Generic.List[object]
foreach ($face in $icoFaces) {
    $tris.Add(@((Normalize3 $icoVerts[$face[0]]),
                (Normalize3 $icoVerts[$face[1]]),
                (Normalize3 $icoVerts[$face[2]])))
}
for ($step = 0; $step -lt 2; $step++) {
    $next = New-Object System.Collections.Generic.List[object]
    foreach ($tri in $tris) {
        $a = $tri[0]; $b = $tri[1]; $c = $tri[2]
        $ab = Normalize3 @((($a[0] + $b[0]) / 2), (($a[1] + $b[1]) / 2), (($a[2] + $b[2]) / 2))
        $bc = Normalize3 @((($b[0] + $c[0]) / 2), (($b[1] + $c[1]) / 2), (($b[2] + $c[2]) / 2))
        $ca = Normalize3 @((($c[0] + $a[0]) / 2), (($c[1] + $a[1]) / 2), (($c[2] + $a[2]) / 2))
        $next.Add(@($a, $ab, $ca))
        $next.Add(@($ab, $b, $bc))
        $next.Add(@($ca, $bc, $c))
        $next.Add(@($ab, $bc, $ca))
    }
    $tris = $next
}

$asteroid = New-MeshBuilder
foreach ($tri in $tris) {
    $corners = @()
    $uvs2 = @()
    foreach ($dir in $tri) {
        $radius = Get-RockRadius $dir
        $corners += , @(($dir[0] * $radius), ($dir[1] * $radius), ($dir[2] * $radius))
        $uvs2 += , @((($dir[0] * 0.5) + 0.5), (($dir[2] * 0.5) + 0.5))
    }
    Add-FlatTriangle $asteroid $corners[0] $corners[1] $corners[2] $uvs2[0] $uvs2[1] $uvs2[2]
}
Write-Gltf $asteroid 'asteroid' "$repo\assets\meshes\asteroid.gltf"
