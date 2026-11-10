# Usage: ./build.ps1 [--pristine]   -> build/merged.hex

$Pristine = $args -contains '--pristine'

$venvPath = "c:/ncs/.venv3.2.4"
$CurrentDirectory = (Get-Location).Path.Replace('\', '/')
$BuildDir = "$CurrentDirectory/build"

if (-not $env:VIRTUAL_ENV) {
    & "$venvPath/Scripts/Activate.ps1"
}

$env:NCS_TOOLCHAIN_VERSION = "v3.2.4"
$env:PATH = "c:/ncs/toolchains/fd21892d0f/opt/bin;" + $env:PATH
$env:ZEPHYR_BASE = "c:/ncs/v3.2.4/zephyr"
Remove-Item Env:\CONF_FILE -ErrorAction SilentlyContinue
Remove-Item Env:\BOARD_ROOT -ErrorAction SilentlyContinue

if ($Pristine -or -not (Test-Path "$BuildDir/build.ninja")) {
    west build -p always -d $BuildDir --board nrf9151dk/nrf9151/ns --sysbuild $CurrentDirectory
} else {
    west build -d $BuildDir
}
exit $LASTEXITCODE
