@echo off
:: ================================================
:: Laptop Prep - All-in-One Script
:: ================================================

net session >nul 2>&1
if %errorLevel% NEQ 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo ============================================
echo Starting laptop prep process...
echo This may take a while depending on updates.
echo ============================================

powershell -NoProfile -ExecutionPolicy Bypass -Command "$c=Get-Content -Raw -LiteralPath '%~f0'; $m='#---POWERSHELL-START---#'; $i=$c.IndexOf($m); $s=$c.Substring($i+$m.Length); Invoke-Expression $s"

echo ============================================
echo Script finished. Check Discord for the log.
echo ============================================
pause
exit /b

#---POWERSHELL-START---#

# ===== CONFIG =====
$WebhookUrl = "https://discord.com/api/webhooks/1559118908588429392/WiHSeWLeBnjaXUsaIg7E1ay53l6sqItrd9M7MxKSL6Sk7GwMp99yPP5vf_wOPmULs7VX"
$LogPath    = "C:\RefurbLogs\$($env:COMPUTERNAME)_prep.log"
$logDir     = Split-Path $LogPath

if (-not (Test-Path $logDir)) { New-Item -Path $logDir -ItemType Directory -Force | Out-Null }

function Write-Log {
    param([string]$Message)
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    "$timestamp - $Message" | Tee-Object -FilePath $LogPath -Append
}

function Send-DiscordLog {
    param([string]$FilePath, [string]$Message)
    $boundary = [System.Guid]::NewGuid().ToString()
    $fileName = Split-Path $FilePath -Leaf
    $fileBytes = [System.IO.File]::ReadAllBytes($FilePath)
    $fileEnc = [System.Text.Encoding]::GetEncoding("ISO-8859-1").GetString($fileBytes)
    $bodyLines = @(
        "--$boundary",
        "Content-Disposition: form-data; name=`"content`"",
        "",
        $Message,
        "--$boundary",
        "Content-Disposition: form-data; name=`"file`"; filename=`"$fileName`"",
        "Content-Type: application/octet-stream",
        "",
        $fileEnc,
        "--$boundary--"
    ) -join "`r`n"
    Invoke-RestMethod -Uri $WebhookUrl -Method Post -ContentType "multipart/form-data; boundary=$boundary" -Body $bodyLines
}

Write-Log "=== Starting laptop prep on $env:COMPUTERNAME ==="

# ---- System specs ----
Write-Log "Collecting system specs..."
$cpu  = Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name
$ram  = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 2)
$disk = Get-CimInstance Win32_DiskDrive | Select-Object Model, Size
$os   = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version

Write-Log "CPU: $cpu"
Write-Log "RAM: $ram GB"
Write-Log "Disk(s): $($disk | ForEach-Object { "$($_.Model) - $([math]::Round($_.Size/1GB,2))GB" })"
Write-Log "OS: $($os.Caption) $($os.Version)"

# ---- Device ID / serials ----
Write-Log "Collecting device identification info..."
$bios        = Get-CimInstance Win32_BIOS
$board       = Get-CimInstance Win32_BaseBoard
$computerSys = Get-CimInstance Win32_ComputerSystem
$uuid        = (Get-CimInstance Win32_ComputerSystemProduct).UUID
$macAddresses = Get-CimInstance Win32_NetworkAdapterConfiguration |
                Where-Object { $_.IPEnabled -eq $true } |
                Select-Object -ExpandProperty MACAddress

$deviceInfo = [PSCustomObject]@{
    ComputerName   = $env:COMPUTERNAME
    Manufacturer   = $computerSys.Manufacturer
    Model          = $computerSys.Model
    BIOS_Serial    = $bios.SerialNumber
    Motherboard_SN = $board.SerialNumber
    System_UUID    = $uuid
    MAC_Addresses  = ($macAddresses -join ", ")
    Asset_Tag      = (Get-CimInstance Win32_SystemEnclosure).SMBIOSAssetTag
}

Write-Log "----- DEVICE ID -----"
Write-Log "Manufacturer:    $($deviceInfo.Manufacturer)"
Write-Log "Model:           $($deviceInfo.Model)"
Write-Log "BIOS Serial:     $($deviceInfo.BIOS_Serial)"
Write-Log "Motherboard SN:  $($deviceInfo.Motherboard_SN)"
Write-Log "System UUID:     $($deviceInfo.System_UUID)"
Write-Log "MAC Address(es): $($deviceInfo.MAC_Addresses)"
Write-Log "Asset Tag:       $($deviceInfo.Asset_Tag)"
Write-Log "---------------------"

$deviceCsvPath = "$logDir\$($env:COMPUTERNAME)_deviceID.csv"
$deviceInfo | Export-Csv -Path $deviceCsvPath -NoTypeInformation

# ---- Battery report ----
Write-Log "Generating battery report..."
powercfg /batteryreport /output "$logDir\battery_report.html" | Out-Null

# ---- Windows Update ----
Write-Log "Checking for PSWindowsUpdate module..."
if (-not (Get-Module -ListAvailable -Name PSWindowsUpdate)) {
    Write-Log "Installing PSWindowsUpdate module..."
    Install-PackageProvider -Name NuGet -Force | Out-Null
    Install-Module PSWindowsUpdate -Force -Confirm:$false
}
Import-Module PSWindowsUpdate

Write-Log "Installing available Windows updates..."
Get-WindowsUpdate -Install -AcceptAll -AutoReboot:$false | Out-File -FilePath $LogPath -Append

Write-Log "=== Prep phase complete ==="

# ---- Send everything to Discord ----
Send-DiscordLog -FilePath $LogPath -Message "✅ Prep log for **$env:COMPUTERNAME**"
Send-DiscordLog -FilePath "$logDir\battery_report.html" -Message "🔋 Battery report for **$env:COMPUTERNAME**"
Send-DiscordLog -FilePath $deviceCsvPath -Message "🆔 Device ID info for **$env:COMPUTERNAME**"