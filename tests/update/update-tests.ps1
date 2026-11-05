param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\build'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\package\Update-GitHub.ps1') -LibraryOnly
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$build = [IO.Path]::GetFullPath($BuildDirectory)
$null = [IO.Directory]::CreateDirectory($build)
Assert-PlainPath $build $true
$fixture = Join-Path $build ('update-fixtures-' + [Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($fixture)
$utf8 = New-Object Text.UTF8Encoding($false)
function Check([bool]$Value, [string]$Message) { if (!$Value) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Check $rejected $Message
}
function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }
function Make-Package([string]$Name, [string]$Version = '2.0.0', [string]$Extra = '') {
    $folder = Join-Path $fixture $Name
    $null = [IO.Directory]::CreateDirectory($folder)
    $revision = Get-Revision $Version
    $manifest = "format=1`nclientVersion=9.9.9.9`nrevision=$revision`ngameHash=$gameHash`nmetadataHash=$metadataHash`n"
    $integrity = ''
    foreach ($file in $script:PayloadFiles) {
        $path = Join-Path $folder $file
        Write-Text $path "synthetic test data $file"
        $hash = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        $integrity += "$file=$hash`n"
        if ($file -in @('BetterDofus.dll', 'BetterDofusLoader.exe', 'BetterDofusAutoLoader.exe')) { $manifest += "$file=$hash`n" }
    }
    Write-Text (Join-Path $folder 'release.ini') $manifest
    Write-Text (Join-Path $folder 'update-files.ini') $integrity
    Write-Text (Join-Path $folder 'LISEZ-MOI.txt') 'fixture'
    Write-Text (Join-Path $folder 'WEBVIEW2-LICENSE.txt') 'fixture'
    $archive = Join-Path $fixture "$Name.zip"
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $folder -File) {
            $entry = "$script:Family$Version/$($file.Name)"
            if ($Extra -and $file.Name -eq 'LISEZ-MOI.txt') { $entry = $Extra }
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entry)
        }
    } finally { $zip.Dispose() }
    return $archive
}
function Asset([string]$Version, [string]$Game, [string]$Metadata, [string]$Digest) {
    $name = "BetterDofus-$Version.zip"
    return [pscustomobject]@{
        draft = $false; prerelease = $false; tag_name = "v$Version"
        body = "Some notes.`n`n<!-- better-dofus game=$Game metadata=$Metadata -->"
        assets = @([pscustomobject]@{
            name = $name; state = 'uploaded'; size = 100
            browser_download_url = "https://github.com/$script:Repository/releases/download/v$Version/$name"
            digest = "sha256:$Digest"
        })
    }
}
try {
    $gameHash = 'a' * 64
    $metadataHash = 'b' * 64
    $digest = 'c' * 64
    # One number per version, in the order of the versions.
    Check ((Get-Revision '2.0.0') -eq 2000000 -and (Get-Revision '2.10.3') -eq 2010003 -and (Get-Revision '2.9.9') -lt (Get-Revision '2.10.0')) 'Versions must compare numerically'
    foreach ($odd in @('2.0', '2.0.0.1', '02.0.0', '2.0.0-beta', '1000.0.0', 'r130', '')) { Check ((Get-Revision $odd) -eq 0) "Malformed version '$odd' accepted" }
    $first = Asset '2.0.0' $gameHash $metadataHash $digest
    $latest = Asset '2.3.0' $gameHash $metadataHash $digest
    $otherGame = Asset '2.4.0' ('d' * 64) $metadataHash $digest
    $otherMetadata = Asset '2.5.0' $gameHash ('d' * 64) $digest
    $draft = Asset '2.6.0' $gameHash $metadataHash $digest; $draft.draft = $true
    $preview = Asset '2.7.0' $gameHash $metadataHash $digest; $preview.prerelease = $true
    Check ((Select-Update @($otherGame, $otherMetadata, $draft, $preview, $latest, $first) $gameHash $metadataHash).revision -eq 2003000) 'Wrong compatible release selected'
    $chosen = Select-Update @($first, (Asset '2.10.0' $gameHash $metadataHash $digest), (Asset '2.9.0' $gameHash $metadataHash $digest)) $gameHash $metadataHash
    Check ($chosen.version -ceq '2.10.0' -and $chosen.url -ceq "https://github.com/$script:Repository/releases/download/v2.10.0/BetterDofus-2.10.0.zip") '2.10.0 must beat 2.9.0'
    # Every rule still holds on its own.
    $external = Asset '2.8.0' $gameHash $metadataHash $digest
    $external.assets[0].browser_download_url = 'https://untrusted.example/archive.zip'
    Check ($null -eq (Select-Update @($external) $gameHash $metadataHash)) 'External URL accepted'
    $otherRepository = Asset '2.8.0' $gameHash $metadataHash $digest
    $otherRepository.assets[0].browser_download_url = 'https://github.com/Altansar69/dofus-enhanced/releases/download/v2.8.0/BetterDofus-2.8.0.zip'
    Check ($null -eq (Select-Update @($otherRepository) $gameHash $metadataHash)) 'Another repository accepted'
    $otherTag = Asset '2.8.0' $gameHash $metadataHash $digest
    $otherTag.tag_name = 'v2.9.0'
    Check ($null -eq (Select-Update @($otherTag) $gameHash $metadataHash)) 'An archive under another tag accepted'
    $renamed = Asset '2.8.0' $gameHash $metadataHash $digest
    $renamed.assets[0].name = 'BetterDofus-2.9.0.zip'
    Check ($null -eq (Select-Update @($renamed) $gameHash $metadataHash)) 'An archive of another version accepted'
    $silent = Asset '2.8.0' $gameHash $metadataHash $digest
    $silent.body = 'Some notes without their build.'
    Check ($null -eq (Select-Update @($silent) $gameHash $metadataHash)) 'A release that does not name its build accepted'
    $noBody = [pscustomobject]@{ draft = $false; prerelease = $false; tag_name = 'v2.8.0'; assets = $silent.assets }
    Check ($null -eq (Select-Update @($noBody) $gameHash $metadataHash)) 'A release without notes accepted'
    $before = Asset '1.21.1' $gameHash $metadataHash $digest
    Check ($null -eq (Select-Update @($before) $gameHash $metadataHash)) 'A release of the former format accepted'
    foreach ($tag in @('2.8.0', 'v2.8', 'v2.8.0-beta', 'v02.8.0', 'V2.8.0', 'r130', '')) {
        $odd = Asset '2.8.0' $gameHash $metadataHash $digest
        $odd.tag_name = $tag
        Check ($null -eq (Select-Update @($odd) $gameHash $metadataHash)) "Malformed tag '$tag' accepted"
    }
    $undigested = Asset '2.8.0' $gameHash $metadataHash $digest; $undigested.assets[0].digest = $null
    Check ($null -eq (Select-Update @($undigested) $gameHash $metadataHash)) 'Missing GitHub digest accepted'
    $oversized = Asset '2.8.0' $gameHash $metadataHash $digest; $oversized.assets[0].size = 33MB
    Check ($null -eq (Select-Update @($oversized) $gameHash $metadataHash)) 'Oversized archive accepted'
    $partial = Asset '2.8.0' $gameHash $metadataHash $digest; $partial.assets[0].state = 'starter'
    Check ($null -eq (Select-Update @($partial) $gameHash $metadataHash)) 'Partial upload accepted'

    $root = Join-Path $fixture 'installed'
    $stage = Join-Path $fixture 'stage'
    $null = [IO.Directory]::CreateDirectory($root)
    $null = [IO.Directory]::CreateDirectory($stage)
    Write-Text (Join-Path $root 'BetterDofus-settings.ini') "githubUpdates=0`nmode=1`n"
    $settingsHash = (Get-FileHash -LiteralPath (Join-Path $root 'BetterDofus-settings.ini')).Hash
    $archive = Make-Package 'valid'
    $hash = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
    Reject { Install-Archive $archive $stage $root '2.0.0' $gameHash $metadataHash ('0' * 64) } 'Bad GitHub digest accepted'
    Reject { Install-Archive $archive $stage $root '2.0.0' $gameHash ('d' * 64) $hash } 'Wrong client metadata accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $root "$($script:Family)2.0.0"))) 'Failed extraction published a partial release'
    # A fresh stage is required after a deliberately rejected archive.
    $stage = Join-Path $fixture 'stage2'; $null = [IO.Directory]::CreateDirectory($stage)
    Check ((Install-Archive $archive $stage $root '2.0.0' $gameHash $metadataHash $hash) -eq 'INSTALLED') 'Verified release was not installed'
    Check ((Install-Archive $archive $stage $root '2.0.0' $gameHash $metadataHash $hash) -eq 'ALREADY_INSTALLED') 'Second check must be idempotent'
    Check ((Get-FileHash -LiteralPath (Join-Path $root 'BetterDofus-settings.ini')).Hash -eq $settingsHash) 'Preferences changed'
    $installed = Join-Path $root "$($script:Family)2.0.0"
    Write-Text (Join-Path $installed 'BetterDofusTreasure.exe') 'tampered'
    Reject { Test-Package $installed 2000000 $gameHash $metadataHash } 'Tampered companion accepted'
    Reject { Install-Archive $archive $stage $root '2.0.0' $gameHash $metadataHash $hash } 'Existing corrupt release silently overwritten'
    $badZip = Make-Package 'traversal' '2.0.1' '../outside.exe'
    $badHash = (Get-FileHash -LiteralPath $badZip).Hash.ToLowerInvariant()
    Reject { Install-Archive $badZip $stage $root '2.0.1' $gameHash $metadataHash $badHash } 'Archive traversal accepted'
    $duplicate = Make-Package 'duplicate' '2.0.2' "$($script:Family)2.0.2/BetterDofusLoader.exe"
    $badHash = (Get-FileHash -LiteralPath $duplicate).Hash.ToLowerInvariant()
    Reject { Install-Archive $duplicate $stage $root '2.0.2' $gameHash $metadataHash $badHash } 'Duplicate path accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $fixture 'outside.exe'))) 'Archive wrote outside staging'

    # Exercise the real orchestration with synthetic installed client files and
    # a deterministic HTTP boundary. No network, registry or game process.
    $client = Join-Path $fixture 'client'
    $metaDirectory = Join-Path $client 'Dofus_Data/il2cpp_data/Metadata'
    $null = [IO.Directory]::CreateDirectory($metaDirectory)
    Write-Text (Join-Path $client 'Dofus.exe') 'fake'
    function Set-Game([string]$Label) {
        Write-Text (Join-Path $client 'GameAssembly.dll') "game $Label"
        Write-Text (Join-Path $metaDirectory 'global-metadata.dat') "metadata $Label"
        $script:gameHash = (Get-FileHash -LiteralPath (Join-Path $client 'GameAssembly.dll')).Hash.ToLowerInvariant()
        $script:metadataHash = (Get-FileHash -LiteralPath (Join-Path $metaDirectory 'global-metadata.dat')).Hash.ToLowerInvariant()
    }
    $exe = Join-Path $client 'Dofus.exe'
    function Release-Of([string]$Version, [string]$Archive) {
        $release = Asset $Version $gameHash $metadataHash (Get-FileHash -LiteralPath $Archive).Hash.ToLowerInvariant()
        $release.assets[0].size = (Get-Item -LiteralPath $Archive).Length
        return $release
    }
    function Get-TrustedDownload([string]$Url, [string]$Destination, [long]$Limit, [Threading.CancellationToken]$Cancellation) {
        $script:requested += $Url
        if ($Url.StartsWith('https://api.github.com/')) {
            if ($script:offline) { throw 'simulated offline' }
            if ($Url.EndsWith('/releases/latest')) { Write-Text $Destination (ConvertTo-Json -InputObject $script:latestRelease -Depth 6) }
            elseif ($script:emptyListing) { Write-Text $Destination '[]' }
            else { Write-Text $Destination (ConvertTo-Json -InputObject @($script:releases) -Depth 6) }
        } else { [IO.File]::Copy($script:archive, $Destination) }
    }
    function Run-Update { $script:requested = @(); return @(Invoke-GitHubUpdate $root $exe) }
    $script:offline = $false; $script:emptyListing = $false

    # A game update: the release for it is found and installed.
    Set-Game 'future'
    $unrelated = Asset '2.0.9' ('d' * 64) $metadataHash $digest
    $script:releases = @($unrelated)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_NO_COMPATIBLE_RELEASE' -and $script:requested.Count -eq 1) 'No compatible release must be told, with nothing downloaded'
    $script:archive = Make-Package 'future210' '2.1.0'
    $script:releases = @(Release-Of '2.1.0' $script:archive)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_INSTALLED 2.1.0' -and $script:requested.Count -eq 2 -and
        $script:requested[1] -ceq "https://github.com/$script:Repository/releases/download/v2.1.0/BetterDofus-2.1.0.zip") 'Full staged update failed'
    Check ((Get-FileHash -LiteralPath (Join-Path $root 'BetterDofus-settings.ini')).Hash -eq $settingsHash) 'Preferences changed by the update'
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_ALREADY_INSTALLED 2.1.0' -and $script:requested.Count -eq 1) 'Unnecessary repeated archive download'
    # A local release newer than what the repository lists stays.
    $script:releases = @($unrelated)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LOCAL_NEWER 2.1.0') 'The local release must stay'
    # A degraded GitHub answers the listing with an empty page: the newest
    # release endpoint still finds the compatible package.
    $script:archive = Make-Package 'newest220' '2.2.0'
    $script:latestRelease = Release-Of '2.2.0' $script:archive
    $script:emptyListing = $true
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LISTING_EMPTY|UPDATE_INSTALLED 2.2.0' -and $script:requested.Count -eq 3 -and
        $script:requested[1] -ceq "https://api.github.com/repos/$script:Repository/releases/latest") 'Empty listing must fall back to the newest release'
    $script:latestRelease = $unrelated
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LISTING_EMPTY|UPDATE_LOCAL_NEWER 2.2.0' -and $script:requested.Count -eq 2) 'An incompatible newest release must not be downloaded'
    $script:emptyListing = $false
    # A wrong digest installs nothing, and a truncated download neither.
    $script:archive = Make-Package 'tampered230' '2.3.0'
    $tampered = Release-Of '2.3.0' $script:archive
    $tampered.assets[0].digest = 'sha256:' + ('0' * 64)
    $script:releases = @($tampered)
    Reject { Invoke-GitHubUpdate $root $exe } 'A corrupt archive was accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $root "$($script:Family)2.3.0"))) 'A corrupt archive left a release'
    $truncated = Release-Of '2.3.0' $script:archive
    $truncated.assets[0].size = $truncated.assets[0].size + 1
    $script:releases = @($truncated)
    Reject { Invoke-GitHubUpdate $root $exe } 'A truncated archive was accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $root "$($script:Family)2.3.0"))) 'A truncated archive left a release'
    # Offline: reported, every local release kept.
    $script:offline = $true
    Reject { Invoke-GitHubUpdate $root $exe } 'Offline must be reported'
    Test-Package (Join-Path $root "$($script:Family)2.2.0") 2002000 $gameHash $metadataHash
    Check (@(Get-ChildItem -LiteralPath $root -Force -Directory | Where-Object Name -Like '.better-dofus-update-*').Count -eq 0) 'Staging cleanup failed'
    Write-Output 'PASS: one version number, compatible selection, trusted assets, atomic install, no overwrite, no traversal, integrity, partial downloads, idempotence, future client, incompatible client, empty-listing fallback, preference retention, offline retention; fixtures only'
} finally {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($fixture)) -ne [IO.Path]::GetFullPath($build) -or
        [IO.Path]::GetFileName($fixture) -notmatch '^update-fixtures-[a-f0-9]{32}$') { throw 'Invalid fixture cleanup target' }
    Assert-PlainPath $fixture $true
    # A file just written can stay briefly held by another process (an antivirus).
    for ($attempt = 1; ; $attempt++) {
        try { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction Stop; break }
        catch { if ($attempt -ge 20) { throw }; Start-Sleep -Milliseconds 250 }
    }
}
