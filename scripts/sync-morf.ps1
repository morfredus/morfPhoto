# Resynchronise la copie vendorée de morfBeacon dans third_party/morf/beacon
# depuis le dépôt source voisin.
#
# Source par défaut : le dossier parent du projet (ex. 01-Travail/).
# Surcharge possible : $env:MORF_SRC_BASE = "C:\chemin\vers\depots"
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot        # racine du projet
$srcBase = if ($env:MORF_SRC_BASE) { $env:MORF_SRC_BASE } else { Split-Path -Parent $root }

function Sync-One($name, $srcDir, $dstDir) {
    if (-not (Test-Path $srcDir)) {
        Write-Error "Source introuvable pour $name : $srcDir (définir MORF_SRC_BASE si ailleurs)"
    }
    Remove-Item -Recurse -Force "$dstDir\include", "$dstDir\src" -ErrorAction SilentlyContinue
    Copy-Item -Recurse "$srcDir\include" "$dstDir\include"
    Copy-Item -Recurse "$srcDir\src"     "$dstDir\src"
    Copy-Item "$srcDir\VERSION" "$dstDir\VERSION"
    $v = (Get-Content "$dstDir\VERSION" -First 1).Trim()
    Write-Output "OK  $name  (version $v)"
}

# morfPhoto n'embarque que morfBeacon. Le depot source peut s'appeler
# « morfBeacon » ou « morfBeacon_travail » : on prend le premier trouve.
$beaconSrc = if (Test-Path "$srcBase\morfBeacon") { "$srcBase\morfBeacon" } else { "$srcBase\morfBeacon_travail" }

Sync-One "morfBeacon" $beaconSrc "$root\third_party\morf\beacon"

# Coeur de deploiement (morfdeploy) : paquet Python, source de verite = depot dedie
# « morfDeploy ». Repli transitoire sur morfTools\lib\morfdeploy tant que la
# migration n'est pas faite.
$deploySrc = $null; $deployVer = $null
if     (Test-Path "$srcBase\morfDeploy\morfdeploy")         { $deploySrc = "$srcBase\morfDeploy\morfdeploy";         $deployVer = "$srcBase\morfDeploy\VERSION" }
elseif (Test-Path "$srcBase\morfDeploy_travail\morfdeploy") { $deploySrc = "$srcBase\morfDeploy_travail\morfdeploy"; $deployVer = "$srcBase\morfDeploy_travail\VERSION" }
elseif (Test-Path "$srcBase\morfTools\lib\morfdeploy")         { $deploySrc = "$srcBase\morfTools\lib\morfdeploy" }
elseif (Test-Path "$srcBase\morfTools_travail\lib\morfdeploy") { $deploySrc = "$srcBase\morfTools_travail\lib\morfdeploy" }

$deployDst = "$root\third_party\morf\morfdeploy"
if ($deploySrc -and (Test-Path $deploySrc)) {
    Remove-Item -Recurse -Force $deployDst -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $deployDst | Out-Null
    Copy-Item -Recurse "$deploySrc\*" $deployDst
    Get-ChildItem -Recurse -Force -Directory $deployDst | Where-Object Name -eq "__pycache__" | Remove-Item -Recurse -Force
    if ($deployVer -and (Test-Path $deployVer)) { Copy-Item $deployVer "$deployDst\VERSION" }
    Write-Output "OK  morfdeploy"
} else {
    Write-Error "Source introuvable pour morfdeploy (morfDeploy ou morfTools\lib\morfdeploy)"
}

Write-Output "Synchronisation terminee. Le CMakeLists vendore n'est pas modifie."
