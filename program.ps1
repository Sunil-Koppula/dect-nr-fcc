# Usage: ./program.ps1 [-Serial <J-Link SN>] [-Modem]
#   -Modem  also flash the DECT NR+ PHY modem firmware (needed once per board,
#           or after the board had LTE modem firmware).
param(
    [string]$Serial = "",
    [switch]$Modem
)

$modem_firmware = "c:\ncs\mfw-nr+-phy_nrf91x1_2.0.0.zip"
$app_hex = "$((Get-Location).Path)/build/merged.hex"

if (-not (Test-Path $app_hex)) {
    Write-Host "Build not found: $app_hex. Run ./build.ps1 first." -ForegroundColor Red
    exit 1
}

$snArgs = @()
if ($Serial) { $snArgs = @('-s', $Serial) }

if ($Modem) {
    Write-Host "Flashing modem firmware $modem_firmware..." -ForegroundColor Yellow
    $nrfutilSn = @()
    if ($Serial) { $nrfutilSn = @('--serial-number', $Serial) }
    nrfutil 91 modem-firmware-upgrade --firmware $modem_firmware @nrfutilSn
    if ($LASTEXITCODE -ne 0) { Write-Host "Modem firmware flash FAILED" -ForegroundColor Red; exit 1 }
}

Write-Host "Flashing $app_hex (chip erase; EEPROM is not touched)..." -ForegroundColor Green
nrfjprog --program $app_hex --chiperase --verify @snArgs
if ($LASTEXITCODE -ne 0) { Write-Host "Flash FAILED" -ForegroundColor Red; exit 1 }

nrfjprog --reset @snArgs
exit $LASTEXITCODE
