# Regenerates the procedural source assets (assets/textures/checker.png,
# assets/meshes/cube.gltf). Windows PowerShell 7+, System.Drawing.
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
