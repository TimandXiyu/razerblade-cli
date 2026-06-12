# Synapse -> EC-firmware opcode capture driver (see ../SKILL.md for the full workflow)
# RUN AS ADMINISTRATOR:  powershell -ExecutionPolicy Bypass -File capture.ps1
# Prerequisite: Wireshark installed WITH the USBPcap component (winget install USBPcap.USBPcap),
# then reboot Windows once -- the capture driver only arms after a reboot.
# Edit $actions below to one label per Synapse knob you want to capture.
# For each labeled action it captures ALL USB root hubs into per-hub .pcap files,
# so every Synapse knob gets its own cleanly-labeled capture segment.
$ErrorActionPreference = 'Stop'

$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
if (!(Test-Path $usbpcap)) {
    Write-Host 'USBPcap not found. Install it (winget install USBPcap.USBPcap or https://desowin.org/usbpcap),' -ForegroundColor Red
    Write-Host 'then REBOOT Windows once (the capture driver only arms after a reboot), then rerun this.' -ForegroundColor Red
    exit 1
}

$outdir = Join-Path $PSScriptRoot 'pcaps'
New-Item -ItemType Directory -Force -Path $outdir | Out-Null

# Inventory all Razer USB devices so the analysis side can match bus addresses.
Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_1532' } |
    Format-List FriendlyName, InstanceId, Status |
    Out-File (Join-Path $outdir 'razer-devices.txt')

# Discover the USBPcap root-hub interfaces.
$hubs = & $usbpcap --extcap-interfaces |
    ForEach-Object { if ($_ -match 'value=(\\\\\.\\USBPcap\d+)') { $Matches[1] } }
if (-not $hubs) { Write-Host 'No USBPcap root hubs found - did you reboot after installing USBPcap?' -ForegroundColor Red; exit 1 }
Write-Host "Capturing root hubs: $($hubs -join ', ')"

$actions = @(
    '00-baseline-idle-15s-touch-nothing',
    '01-sanity-mode-balanced-to-gaming',
    '02-mode-enter-CUSTOM',
    '03-cpu-boost-LOW',
    '04-cpu-boost-MEDIUM',
    '05-cpu-boost-HIGH',
    '06-cpu-boost-BOOST-if-present',
    '07-gpu-boost-LOW',
    '08-gpu-boost-MEDIUM',
    '09-gpu-boost-HIGH',
    '10-custom-fan-slider-change',
    '11-exit-custom-back-to-balanced',
    '12-battery-health-optimizer-ON-80pct',
    '13-charge-limit-change-to-65pct',
    '14-battery-health-optimizer-OFF',
    '15-kbd-brightness-change'
)

foreach ($a in $actions) {
    Write-Host ''
    Write-Host ">>> NEXT SEGMENT: $a" -ForegroundColor Cyan
    Read-Host '    Press Enter to START capturing, then perform ONLY that action in Synapse'
    $procs = @()
    $i = 0
    foreach ($h in $hubs) {
        $i++
        $f = Join-Path $outdir "$a-hub$i.pcap"
        $procs += Start-Process -FilePath $usbpcap -ArgumentList "-d $h -o `"$f`" -A" -PassThru -WindowStyle Hidden
    }
    Start-Sleep -Seconds 1
    Read-Host '    ...do the action now (give it ~3s to settle). Press Enter to STOP this segment'
    $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Write-Host "    saved segment $a" -ForegroundColor Green
}

Write-Host ''
Write-Host "All segments captured. Files are in $outdir." -ForegroundColor Green
Write-Host 'Reboot back into Linux and decode the pcaps (see SKILL.md, Phase 2).' -ForegroundColor Green
