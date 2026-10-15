Param ($VersionFile)

# ddrawcompat-lite: the version comes from the VERSION file in the repository root.
$VersionFilePath = Join-Path $PSScriptRoot '..\VERSION'
If (Test-Path $VersionFilePath) {
    $VersionString = 'v' + (Get-Content $VersionFilePath -Raw).Trim()
}

If ( ($VersionString -is [String]) -and ($VersionString -match '^v[0-9]+\.[0-9]+\.[0-9]+(-.*)?$') ) {
    $VersionNumber = $VersionString.Split('-', 2)[0].Substring(1).Replace('.', ',') + ",0"
}
Else {
    $VersionString = 'unknown'
    $VersionNumber = '0,0,0,0'
}

$FileContent = @"
#define VERSION_NUMBER $VersionNumber
#define VERSION_STRING "$VersionString"
"@

Try {
    $PrevFileContent = [System.IO.File]::ReadAllText($VersionFile).Trim()
}
Catch [System.IO.FileNotFoundException] {
}

If ( $FileContent -eq $PrevFileContent ) {
    Write-Host "Version: $VersionString (same as previous build)"
}
Else {
    Write-Host "Version: $VersionString (differs from previous build)"
    $FileContent | Out-File -FilePath $VersionFile
}
