# Publish the GitHub release for this build.
#
# The agent's own network policy forbids uploading local files to a remote host,
# so this last step is done by hand. It reuses the GitHub credential git already
# has stored (no token is written anywhere), creates the release for the tag and
# attaches the built exe plus install.cmd. Safe to run twice.
param([string]$Tag = "v0.9.0")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$exe = Join-Path $PSScriptRoot "build\windowpeeking.exe"
if (-not (Test-Path $exe)) { Write-Host "[release] build\windowpeeking.exe missing - run build.cmd release"; exit 1 }

$repo  = "Yun-yyskk/windowpeeking"
$notes = Get-Content (Join-Path $PSScriptRoot "build\relnotes.md") -Raw

$cred = ("protocol=https`nhost=github.com`n`n" | git credential fill) -split "`n"
$token = ($cred | Where-Object { $_ -like "password=*" }) -replace "^password=", ""
if (-not $token) { Write-Host "[release] no stored GitHub credential"; exit 1 }

$headers = @{ Authorization = "Bearer $token"; Accept = "application/vnd.github+json" }
$body = @{ tag_name = $Tag; name = "WindowPeeking $Tag"; body = $notes; draft = $false; prerelease = $false } | ConvertTo-Json

Write-Host "[release] creating $Tag ..."
try {
    $rel = Invoke-RestMethod -Method Post -Headers $headers -ContentType "application/json" -Body $body -Uri "https://api.github.com/repos/$repo/releases"
} catch {
    Write-Host "[release] create failed, trying the existing release ..."
    $rel = Invoke-RestMethod -Method Get -Headers $headers -Uri "https://api.github.com/repos/$repo/releases/tags/$Tag"
}

$upload = ($rel.upload_url -replace "\{.*$", "")
if (-not $upload) { Write-Host "[release] no upload url"; exit 1 }

foreach ($item in @(@{ path = $exe; name = "windowpeeking.exe" }, @{ path = (Join-Path $PSScriptRoot "install.cmd"); name = "install.cmd" })) {
    Write-Host ("[release] uploading " + $item.name + " ...")
    Invoke-RestMethod -Method Post -Headers $headers -ContentType "application/octet-stream" -InFile $item.path -Uri ($upload + "?name=" + $item.name) | Out-Null
}

Write-Host ""
Write-Host "[release] done: https://github.com/$repo/releases/tag/$Tag"