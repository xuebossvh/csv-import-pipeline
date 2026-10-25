param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$InputFile,
    [Parameter(Mandatory=$true)][string]$OutputPrefix,
    [string]$Configuration = 'unknown',
    [string]$SourceCommit = 'working-tree',
    [int]$Repeat = 1,
    [int]$SampleMilliseconds = 100,
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
if ($Repeat -lt 1 -or $SampleMilliseconds -lt 10 -or $TimeoutSeconds -lt 1) { throw 'Invalid measurement parameters.' }
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$binaryPath = if ([System.IO.Path]::IsPathRooted($Binary)) { $Binary } else { Join-Path $ProjectRoot $Binary }
$inputPath = if ([System.IO.Path]::IsPathRooted($InputFile)) { $InputFile } else { Join-Path $ProjectRoot $InputFile }
$prefix = if ([System.IO.Path]::IsPathRooted($OutputPrefix)) { $OutputPrefix } else { Join-Path $ProjectRoot $OutputPrefix }
$reportArgument = $OutputPrefix + '-report.txt'
foreach ($argument in @($InputFile,$reportArgument)) { if ($argument.Contains('"') -or $argument.Contains([char]10)) { throw 'Quotes/newlines are not supported in measurement paths.' } }
New-Item -ItemType Directory -Path (Split-Path $prefix -Parent) -Force | Out-Null
$inputItem = Get-Item -LiteralPath $inputPath
$inputHash = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
$binaryHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $binaryPath
$startInfo.Arguments = '"' + $InputFile + '" ' + $Repeat + ' "' + $reportArgument + '"'
$startInfo.WorkingDirectory = $ProjectRoot
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$child = New-Object System.Diagnostics.Process
$child.StartInfo = $startInfo
$started = [DateTimeOffset]::Now
$clock = [System.Diagnostics.Stopwatch]::StartNew()
[void]$child.Start()
$stdoutTask = $child.StandardOutput.ReadToEndAsync()
$stderrTask = $child.StandardError.ReadToEndAsync()
$maxWorkingSet = [long]0
$maxPrivate = [long]0
$samples = 0
$timedOut = $false
while (-not $child.HasExited) {
    $child.Refresh()
    try {
        $maxWorkingSet = [Math]::Max($maxWorkingSet,[long]$child.PeakWorkingSet64)
        $maxPrivate = [Math]::Max($maxPrivate,[long]$child.PrivateMemorySize64)
        $samples++
    } catch { }
    if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
        $timedOut = $true
        $child.Kill()
        break
    }
    [System.Threading.Thread]::Sleep($SampleMilliseconds)
}
$child.WaitForExit()
$clock.Stop()
try { $maxWorkingSet = [Math]::Max($maxWorkingSet,[long]$child.PeakWorkingSet64) } catch { }
$cpuSeconds = try { $child.TotalProcessorTime.TotalSeconds } catch { $null }
$programExit = $child.ExitCode
$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result
$child.Dispose()
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($prefix + '-stdout.txt',$stdout,$utf8)
[System.IO.File]::WriteAllText($prefix + '-stderr.txt',$stderr,$utf8)
$runs = @()
foreach ($match in [regex]::Matches($stdout,'Run ([0-9]+): ([0-9]+) rows in ([0-9.eE+-]+) ms(?: \(([0-9]+) skipped\))?')) {
    $runs += [PSCustomObject]@{
        Run=[int]$match.Groups[1].Value
        RowsImported=[long]$match.Groups[2].Value
        Milliseconds=[double]::Parse($match.Groups[3].Value,[Globalization.CultureInfo]::InvariantCulture)
        RowsSkipped=if ($match.Groups[4].Success) { [long]$match.Groups[4].Value } else { 0 }
    }
}
$result = [PSCustomObject]@{
    Started=$started.ToString('o'); Configuration=$Configuration; SourceCommit=$SourceCommit
    Binary=$Binary; BinarySHA256=$binaryHash; InputFile=$InputFile
    InputBytes=$inputItem.Length; InputSHA256=$inputHash; Repeats=$Repeat
    ExitCode=$programExit; TimedOut=$timedOut; WallSeconds=$clock.Elapsed.TotalSeconds
    CPUSeconds=$cpuSeconds; PeakWorkingSetBytes=$maxWorkingSet
    PeakPrivateBytesSampled=$maxPrivate; SampleMilliseconds=$SampleMilliseconds; Samples=$samples
    CachePolicy='Input hash read immediately before launch; OS file cache was not flushed.'
    Runs=$runs
}
$json = $result | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($prefix + '.json',$json,$utf8)
Write-Output $json
if ($timedOut) { exit 124 }
exit $programExit
