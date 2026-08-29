<#
.SYNOPSIS
  Creates the glsplay host VM and its firewall rules on GCP.

.DESCRIPTION
  Run this from your own machine, not the VM. It provisions a g2-standard-4
  (1x NVIDIA L4) running Windows Server 2022 in asia-south1, opens the media
  and signaling ports, and prints the next steps.

  Requires: gcloud CLI, authenticated, with a project selected.

  GPU quota is the long pole. If NVIDIA_L4_GPUS in the target region is 0,
  this will fail at instance creation - request the quota first and wait for
  approval, which can take from minutes to a couple of days.

.EXAMPLE
  .\provision-vm.ps1 -ProjectId my-project
  .\provision-vm.ps1 -ProjectId my-project -InstanceName glsplay-1 -DiskGb 200
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ProjectId,

  [string]$InstanceName = 'glsplay-host',
  [string]$Region = 'asia-south1',
  [string]$Zone = 'asia-south1-c',
  [string]$MachineType = 'g2-standard-4',
  [int]$DiskGb = 100,

  # UDP range carrying RTP/RTCP. Must match GLSPLAY min-port/max-port.
  [int]$MediaPortStart = 50000,
  [int]$MediaPortEnd = 50100,
  [int]$SignalingPort = 8080,

  # Restrict inbound to your own address. 0.0.0.0/0 leaves the signaling port
  # exposed to the internet, which is only acceptable behind a strong secret.
  [string]$SourceRange = '0.0.0.0/0',

  [switch]$SkipFirewall
)

$ErrorActionPreference = 'Stop'

function Write-Step { param([string]$Message) Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Write-Ok   { param([string]$Message) Write-Host "    $Message" -ForegroundColor Green }
function Write-Warn { param([string]$Message) Write-Host "    $Message" -ForegroundColor Yellow }

# --- preflight --------------------------------------------------------------

Write-Step 'Checking gcloud'
if (-not (Get-Command gcloud -ErrorAction SilentlyContinue)) {
  throw 'gcloud CLI not found. Install the Google Cloud SDK and run: gcloud auth login'
}
$account = (gcloud config get-value account 2>$null)
if (-not $account -or $account -eq '(unset)') {
  throw 'gcloud is not authenticated. Run: gcloud auth login'
}
Write-Ok "authenticated as $account"

Write-Step "Checking L4 quota in $Region"
# A zero limit here is the single most common reason provisioning fails, and
# the error gcloud returns at create time does not make the cause obvious.
$quotaJson = gcloud compute regions describe $Region --project $ProjectId --format=json | ConvertFrom-Json
$gpuQuota = $quotaJson.quotas | Where-Object { $_.metric -eq 'NVIDIA_L4_GPUS' }
if ($null -eq $gpuQuota) {
  Write-Warn "NVIDIA_L4_GPUS quota not reported for $Region - L4 may be unavailable in this region"
} elseif ($gpuQuota.limit -lt 1) {
  Write-Host ''
  Write-Host '  BLOCKED: NVIDIA_L4_GPUS quota is 0 in this region.' -ForegroundColor Red
  Write-Host '  Request an increase before continuing:' -ForegroundColor Red
  Write-Host "    https://console.cloud.google.com/iam-admin/quotas?project=$ProjectId" -ForegroundColor Red
  Write-Host '    Filter for "NVIDIA L4 GPUs", select the region, and request at least 1.'
  Write-Host ''
  throw 'Insufficient GPU quota.'
} else {
  Write-Ok "NVIDIA_L4_GPUS limit=$($gpuQuota.limit) usage=$($gpuQuota.usage)"
}

# --- firewall ---------------------------------------------------------------

if (-not $SkipFirewall) {
  Write-Step 'Creating firewall rules'

  $rules = @(
    @{ Name = 'glsplay-media';     Rules = "udp:$MediaPortStart-$MediaPortEnd"; Desc = 'glsplay WebRTC media (RTP/RTCP)' },
    @{ Name = 'glsplay-signaling'; Rules = "tcp:$SignalingPort";                Desc = 'glsplay WebSocket signaling' },
    @{ Name = 'glsplay-rdp';       Rules = 'tcp:3389';                          Desc = 'RDP for initial setup only' }
  )

  foreach ($rule in $rules) {
    $exists = gcloud compute firewall-rules describe $rule.Name --project $ProjectId --format='value(name)' 2>$null
    if ($exists) {
      Write-Ok "$($rule.Name) already exists"
      continue
    }
    gcloud compute firewall-rules create $rule.Name `
      --project $ProjectId `
      --direction=INGRESS `
      --priority=1000 `
      --network=default `
      --action=ALLOW `
      --rules=$($rule.Rules) `
      --source-ranges=$SourceRange `
      --target-tags=glsplay-host `
      --description=$($rule.Desc) | Out-Null
    Write-Ok "created $($rule.Name) -> $($rule.Rules)"
  }

  if ($SourceRange -eq '0.0.0.0/0') {
    Write-Warn 'Source range is 0.0.0.0/0. Narrow this to your own IP once you have tested.'
  }
}

# --- instance ---------------------------------------------------------------

Write-Step "Creating $InstanceName ($MachineType) in $Zone"

$existing = gcloud compute instances describe $InstanceName --zone $Zone --project $ProjectId --format='value(name)' 2>$null
if ($existing) {
  Write-Warn "$InstanceName already exists - skipping creation"
} else {
  # Notes on the flags that are not obvious:
  #   --maintenance-policy=TERMINATE is mandatory for GPU instances; they
  #     cannot be live-migrated.
  #   G2 machine types include their L4 implicitly, so no --accelerator flag.
  #   --enable-display-device gives Windows a virtual display head. It is not
  #     a substitute for the IDD driver for capture, but it makes the console
  #     usable before the IDD driver is installed.
  gcloud compute instances create $InstanceName `
    --project $ProjectId `
    --zone $Zone `
    --machine-type $MachineType `
    --maintenance-policy=TERMINATE `
    --restart-on-failure `
    --enable-display-device `
    --image-family=windows-2022 `
    --image-project=windows-cloud `
    --boot-disk-size="${DiskGb}GB" `
    --boot-disk-type=pd-balanced `
    --boot-disk-device-name=$InstanceName `
    --tags=glsplay-host `
    --scopes=https://www.googleapis.com/auth/cloud-platform `
    --metadata=enable-oslogin=FALSE | Out-Null

  Write-Ok "instance created"
}

$externalIp = gcloud compute instances describe $InstanceName --zone $Zone --project $ProjectId `
  --format='value(networkInterfaces[0].accessConfigs[0].natIP)'

# --- next steps -------------------------------------------------------------

Write-Host ''
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host " glsplay host provisioned" -ForegroundColor White
Write-Host '-------------------------------------------------------------' -ForegroundColor DarkGray
Write-Host "  instance   : $InstanceName"
Write-Host "  zone       : $Zone"
Write-Host "  external IP: $externalIp"
Write-Host "  media UDP  : $MediaPortStart-$MediaPortEnd"
Write-Host ''
Write-Host ' Next steps:' -ForegroundColor Cyan
Write-Host ''
Write-Host '  1. Set a Windows password:'
Write-Host "       gcloud compute reset-windows-password $InstanceName --zone $Zone --project $ProjectId"
Write-Host ''
Write-Host '  2. RDP in and copy the vm-scripts folder across, then run as Administrator:'
Write-Host '       .\setup-gcp-vm.ps1'
Write-Host '       .\install-virtual-display.ps1'
Write-Host '       .\setup-firewall.ps1'
Write-Host ''
Write-Host '  3. Reboot, then verify before doing anything else:'
Write-Host '       .\check-environment.ps1'
Write-Host ''
Write-Host '  IMPORTANT: the host must run in the CONSOLE session, not RDP.' -ForegroundColor Yellow
Write-Host '  Desktop Duplication captures whichever session owns the display,' -ForegroundColor Yellow
Write-Host '  and an RDP session has no NVENC. check-environment.ps1 tests this.' -ForegroundColor Yellow
Write-Host ''
Write-Host '  Stop the instance when idle - you pay for the L4 by the hour:' -ForegroundColor DarkGray
Write-Host "       gcloud compute instances stop $InstanceName --zone $Zone --project $ProjectId"
Write-Host ''
