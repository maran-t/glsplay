<#
.SYNOPSIS
  Opens the Windows Firewall ports glsplay needs. Run on the VM as Administrator.

.DESCRIPTION
  Windows Firewall and the GCP VPC firewall are independent layers - traffic
  must be permitted by both. provision-vm.ps1 handles the VPC side; this
  handles the guest side.

  The media range must match the host's --min-port/--max-port, because ICE
  will otherwise gather candidates on ports nothing has opened and the
  connection will fail at exactly the point everything looks correct.
#>

[CmdletBinding()]
param(
  [int]$MediaPortStart = 50000,
  [int]$MediaPortEnd = 50100,
  [int]$SignalingPort = 8080,
  [switch]$Remove
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
      ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script as Administrator.'
}

$rules = @(
  @{
    Name     = 'glsplay-media-udp'
    Protocol = 'UDP'
    Port     = "$MediaPortStart-$MediaPortEnd"
    Desc     = 'glsplay WebRTC media (RTP/RTCP) - PRD section 4.5'
  },
  @{
    Name     = 'glsplay-signaling-tcp'
    Protocol = 'TCP'
    Port     = "$SignalingPort"
    Desc     = 'glsplay WebSocket signaling broker'
  }
)

if ($Remove) {
  foreach ($rule in $rules) {
    Remove-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue
    Write-Host "removed $($rule.Name)" -ForegroundColor Yellow
  }
  return
}

foreach ($rule in $rules) {
  Remove-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue

  New-NetFirewallRule `
    -DisplayName $rule.Name `
    -Description $rule.Desc `
    -Direction Inbound `
    -Action Allow `
    -Protocol $rule.Protocol `
    -LocalPort $rule.Port `
    -Profile Any `
    -Enabled True | Out-Null

  Write-Host "  [ok] $($rule.Name)  $($rule.Protocol)/$($rule.Port)" -ForegroundColor Green
}

# Outbound is allowed by default on Windows Server, but an explicit rule makes
# the intent visible and survives a hardened baseline being applied later.
Remove-NetFirewallRule -DisplayName 'glsplay-media-udp-out' -ErrorAction SilentlyContinue
New-NetFirewallRule `
  -DisplayName 'glsplay-media-udp-out' `
  -Description 'glsplay outbound WebRTC media' `
  -Direction Outbound `
  -Action Allow `
  -Protocol UDP `
  -LocalPort "$MediaPortStart-$MediaPortEnd" `
  -Profile Any `
  -Enabled True | Out-Null
Write-Host "  [ok] glsplay-media-udp-out  UDP/$MediaPortStart-$MediaPortEnd" -ForegroundColor Green

Write-Host ''
Write-Host 'Windows Firewall configured.' -ForegroundColor Cyan
Write-Host 'Confirm the GCP VPC rules exist too - both layers must allow the traffic:' -ForegroundColor Yellow
Write-Host '  gcloud compute firewall-rules list --filter="name~glsplay"'
Write-Host ''
