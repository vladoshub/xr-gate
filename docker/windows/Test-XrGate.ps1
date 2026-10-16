#Requires -Version 5.1
[CmdletBinding()]
param([ValidateRange(1, 65535)][int]$Port = 45810)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$client = New-Object System.Net.Sockets.TcpClient
try {
    $connect = $client.ConnectAsync('127.0.0.1', $Port)
    if (-not $connect.Wait(5000)) { throw 'TCP connect timeout. Complete xr_client startup in its console first.' }
    [void]$connect.GetAwaiter().GetResult()
    $reader = New-Object System.IO.StreamReader($client.GetStream())
    try {
        $pending = $reader.ReadLineAsync()
        if (-not $pending.Wait(8000)) { throw 'Timed out waiting for the XR Gate hello.' }
        $line = $pending.GetAwaiter().GetResult()
        if (-not $line) { throw 'Connection closed before hello.' }
        $hello = $line | ConvertFrom-Json
        if ($hello.type -ne 'hello' -or $hello.protocol -ne 'xr_gate_app_v1') { throw "Unexpected hello: $line" }
        Write-Host $line
        $pending = $reader.ReadLineAsync()
        if (-not $pending.Wait(8000)) { throw 'Timed out waiting for a frame.' }
        $line = $pending.GetAwaiter().GetResult()
        if (-not $line) { throw 'Connection closed before frame.' }
        $frame = $line | ConvertFrom-Json
        if ($frame.type -ne 'frame') { throw 'Unexpected message type.' }
        Write-Host "OK Windows localhost:$Port ; frame sequence=$($frame.sequence). Check pose_valid separately."
    } finally { $reader.Dispose() }
} finally { $client.Dispose() }
