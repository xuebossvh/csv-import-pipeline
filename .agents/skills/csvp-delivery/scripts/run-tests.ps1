param([string]$ProjectRoot,[string]$Binary,[string]$OutputPrefix,[string]$Configuration)
$ErrorActionPreference='Stop'
$prefix=Join-Path $ProjectRoot $OutputPrefix
New-Item -ItemType Directory -Path (Split-Path $prefix -Parent) -Force | Out-Null
# Google Test 1.14's narrow Windows XML file API cannot reliably use this
# repository's Unicode path. Collect XML in a unique ASCII temporary folder.
$xmlDirectory=Join-Path ([IO.Path]::GetTempPath()) ('csvp-v11-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $xmlDirectory | Out-Null
$xmlPath=Join-Path $xmlDirectory 'tests.xml'
$info=New-Object System.Diagnostics.ProcessStartInfo
$info.FileName=Join-Path $ProjectRoot $Binary
$info.Arguments='"--gtest_output=xml:'+$xmlPath+'"'
$info.WorkingDirectory=$ProjectRoot
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
$info.EnvironmentVariables['ASAN_OPTIONS']='halt_on_error=1:abort_on_error=1'
$p=New-Object System.Diagnostics.Process
$p.StartInfo=$info
[void]$p.Start()
$outTask=$p.StandardOutput.ReadToEndAsync()
$errTask=$p.StandardError.ReadToEndAsync()
$p.WaitForExit()
$exitCode=$p.ExitCode
$stdout=$outTask.Result
$stderr=$errTask.Result
$p.Dispose()
if(Test-Path -LiteralPath $xmlPath) {
    Copy-Item -LiteralPath $xmlPath -Destination ($prefix+'.xml')
    Remove-Item -LiteralPath $xmlPath
}
[IO.Directory]::Delete($xmlDirectory)
$utf8=New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($prefix+'.txt',"Configuration: $Configuration`nBinary: $Binary`nExitCode: $exitCode`nASAN_OPTIONS: halt_on_error=1:abort_on_error=1`n`n"+$stdout+"`nSTDERR:`n"+$stderr,$utf8)
$stdout -split '\r?\n' | Where-Object { $_ -match '\[==========\]|\[  PASSED  \]|\[  FAILED  \]|DISABLED' } | Write-Output
if($exitCode -ne 0) { Write-Output $stdout; Write-Output $stderr }
exit $exitCode
