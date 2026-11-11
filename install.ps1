# Copies B2KShake.ofx.bundle into the shared OFX plugin folder (run as Administrator).
# Close VEGAS first; it rescans plugins on next start.
$src = Join-Path $PSScriptRoot 'B2KShake.ofx.bundle'
$dst = 'C:\Program Files\Common Files\OFX\Plugins'
if (-not (Test-Path (Join-Path $src 'Contents\Win64\B2KShake.ofx'))) { throw 'Build first (build.bat).' }
# remove the earlier name of this plugin
$old = Join-Path $dst 'ShakeFX.ofx.bundle'
if (Test-Path $old) { Remove-Item $old -Recurse -Force }
Copy-Item $src $dst -Recurse -Force
Write-Host "Installed to $dst\B2KShake.ofx.bundle"
