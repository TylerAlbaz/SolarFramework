param([string]$SrcDir=".", [string]$OutDir=".")
$ErrorActionPreference = "Stop"

function Resolve-Glslc {
  if ($env:VULKAN_SDK) {
    $p = Join-Path $env:VULKAN_SDK "Bin\glslc.exe"
    if (Test-Path $p) { return $p }
  }
  $p = (Get-Command glslc.exe -ErrorAction SilentlyContinue)?.Source
  if ($p) { return $p }
  throw "glslc.exe not found. Install the Vulkan SDK or add it to PATH."
}

$glslc = Resolve-Glslc
Write-Host "Using glslc: $glslc"

$SrcDir = (Resolve-Path $SrcDir).Path
$OutDir = (Resolve-Path $OutDir).Path
[Environment]::CurrentDirectory = $SrcDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Push-Location $SrcDir
try {
  # IMPORTANT: don't use -Include here; it needs -Recurse or wildcarded -Path.
  $shaders = Get-ChildItem -File | Where-Object {
    $_.Extension -in '.vert', '.frag', '.comp'
  }

  if (!$shaders) {
    Write-Host "No shaders in $SrcDir"
    exit 0
  }



  function To-Inc([string]$spv, [string]$inc) {
    $bytes = [IO.File]::ReadAllBytes($spv)
    if (($bytes.Length % 4) -ne 0) { throw "SPIR-V not dword sized: $spv" }
    $sw = New-Object IO.StreamWriter($inc, $false, [Text.Encoding]::ASCII)
    try {
      for ($i=0; $i -lt $bytes.Length; $i+=4) {
        $u32 = [BitConverter]::ToUInt32($bytes,$i)
        $sw.Write(("0x{0:x8}u," -f $u32))
        if (((($i/4)+1) % 8) -eq 0) { $sw.WriteLine() }
      }
      $sw.WriteLine()
    } finally { $sw.Dispose() }
    Write-Host "Wrote $inc"
  }

  foreach ($s in $shaders) {
    $inName = $s.Name                    # simple filename (no spaces)
    $base   = [IO.Path]::GetFileNameWithoutExtension($inName)
    $spv    = Join-Path $OutDir ($base + ".spv")
    $inc    = Join-Path $OutDir ($base + ".spv.inc")

    Write-Host "Compiling $inName -> $(Split-Path $spv -Leaf)"
    $args = @('--', $inName, '-O', '-o', $spv)   # end-of-options avoids arg parsing weirdness
    $p = Start-Process -FilePath $glslc -ArgumentList $args -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0 -or -not (Test-Path $spv)) {
      throw "glslc failed for $inName (exit $($p.ExitCode))"
    }

    To-Inc $spv $inc
  }
}
finally { Pop-Location }
exit 0