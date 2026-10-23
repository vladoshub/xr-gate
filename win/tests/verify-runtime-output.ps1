#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'XrGate-Wsl.ps1') -Action Help *> $null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
$script:Shell = (Get-Process -Id $PID).Path
function Get-Tool($Name) { return $script:Shell }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('xr-gate-runtime-output-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
$script:StateDir = $temporary
try {
    $record = [pscustomobject]@{Token=('a' * 32)}
    $files = Get-RuntimeWindowFiles $record
    $command = '$OutputEncoding=[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false); [Console]::WriteLine("runtime started"); [Console]::Error.WriteLine("camera unavailable"); exit 7'
    $code = Invoke-LoggedRuntime -Arguments @('-NoLogo','-NoProfile','-Command',$command) -Files $files
    Assert ($code -eq 7) 'Runtime exit code was lost.'
    Assert ((Get-Content -Raw -LiteralPath $files.Output).Contains('runtime started')) 'Hidden runtime stdout was lost.'
    Assert ((Get-Content -Raw -LiteralPath $files.Error).Contains('camera unavailable')) 'Hidden runtime stderr was lost.'
    [IO.File]::WriteAllText($files.Result, '{"message":"fixture runtime failed"}')
    $exit = Get-RuntimeExitObservation $record
    Assert ($exit.message -eq 'fixture runtime failed' -and $exit.log_tail.Count -eq 2) 'Exited runtime observation lost its failure or current-token log.'
    foreach ($line in $exit.log_tail) {
        Assert ($line -is [string] -and !$line.PSObject.Properties['PSPath']) 'Runtime log lines retained provider metadata.'
    }
    $status = @{runtime_log_tail=$exit.log_tail} | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    Assert ($status.runtime_log_tail.Count -eq 2) 'Runtime status lost its log array during JSON serialization.'
    foreach ($line in $status.runtime_log_tail) {
        Assert ($line -is [string]) 'Runtime status serialized a log line as an object instead of a string.'
    }
    [IO.File]::WriteAllLines($files.Output, [string[]]@(1..80 | ForEach-Object { 'frame ' + $_ }), (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($files.Error, '[ERROR] camera unavailable', (New-Object Text.UTF8Encoding($false)))
    $exit = Get-RuntimeExitObservation $record
    Assert ($exit.message -eq '[ERROR] camera unavailable' -and $exit.log_tail.Count -eq 31 -and $exit.log_tail[0] -eq 'frame 51') 'Runtime failure tails were unbounded or lost the actual error.'

    # Exercise the native process boundary, including paths and payloads requiring
    # quotes. On Windows also check the raw WSL option prefix, which wsl.exe
    # parses before CommandLineToArgvW handles the Linux executable and arguments.
    $probe = Join-Path $temporary "O'Brien native arguments.ps1"
    $request = To-Base64Json @{user='vlados';settings=@{XR_DEVICE_NAME='Generic XR';XR_TCP_PORT='45810'}}
    $expected = @('--distribution','Ubuntu-24.04','--user','vlados','--exec','python3',
        ("/mnt/c/" + [char]0x041D + [char]0x043E + [char]0x0432 + [char]0x0430 + [char]0x044F + " O'Brien XR Gate/linux/xr_gate_wsl.py"),'run',$request,'',
        'a"b', 'C:\folder with space\', 'C:\simple\', 'literal; & | $() `<>', "line1`nline2")
    $encoded = To-Base64Json $expected
    $body = @'
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$expected=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__EXPECTED__')) | ConvertFrom-Json
if ($args.Count -ne $expected.Count) { throw 'Native argument count changed.' }
for ($index=0; $index -lt $expected.Count; $index++) {
    if ([string]$args[$index] -cne [string]$expected[$index]) { throw ('Native argument changed at index ' + $index) }
}
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $raw=[Environment]::CommandLine
    foreach ($option in @('--distribution','--user','--exec')) {
        if ($raw.Contains('"' + $option + '"')) { throw ('WSL option was quoted in the raw command line: ' + $option) }
    }
}
[Console]::WriteLine('runtime arguments preserved')
exit 0
'@
    [IO.File]::WriteAllText($probe, $body.Replace('__EXPECTED__',$encoded), (New-Object Text.UTF8Encoding($true)))
    $probeFiles = Get-RuntimeWindowFiles ([pscustomobject]@{Token=('b' * 32)})
    $code = Invoke-LoggedRuntime -Arguments (@('-NoLogo','-NoProfile','-File',$probe) + $expected) -Files $probeFiles
    Assert ($code -eq 0 -and (Get-Content -Raw -LiteralPath $probeFiles.Output).Contains('runtime arguments preserved')) 'Hidden runtime changed native arguments or quoted WSL options.'
    Assert (!(Get-RuntimeWindowFiles ([pscustomobject]@{Token='../other'}))) 'Runtime log paths trusted an invalid token.'
    Write-Host 'OK: hidden runtime preserves native arguments and WSL options, owns UTF-8 log pipes and reports bounded plain-string JSON failure tails.'
} finally { Remove-Item -Recurse -Force -LiteralPath $temporary }
