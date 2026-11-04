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
function Make-Package([string]$Name, [int]$Revision = 75, [string]$Extra = '') {
    $folder = Join-Path $fixture $Name
    $null = [IO.Directory]::CreateDirectory($folder)
    $manifest = "format=1`nclientVersion=9.9.9.9`nrevision=$Revision`ngameHash=$gameHash`nmetadataHash=$metadataHash`n"
    $integrity = ''
    foreach ($file in $script:PayloadFiles) {
        $path = Join-Path $folder $file
        Write-Text $path "synthetic test data $file"
        $hash = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        $integrity += "$file=$hash`n"
        if ($file -in @('DofusMiniatures.dll', 'ModuleLoader.exe', 'ModuleAutoLoader.exe')) { $manifest += "$file=$hash`n" }
    }
    Write-Text (Join-Path $folder 'release.ini') $manifest
    Write-Text (Join-Path $folder 'update-files.ini') $integrity
    Write-Text (Join-Path $folder 'LISEZ-MOI.txt') 'fixture'
    Write-Text (Join-Path $folder 'WEBVIEW2-LICENSE.txt') 'fixture'
    $archive = Join-Path $fixture "$Name.zip"
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $folder -File) {
            $entry = "$script:Family$Revision/$($file.Name)"
            if ($Extra -and $file.Name -eq 'LISEZ-MOI.txt') { $entry = $Extra }
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entry)
        }
    } finally { $zip.Dispose() }
    return $archive
}
function Retag($Release, [string]$Tag) {
    $Release.tag_name = $Tag
    $Release.assets[0].browser_download_url = "https://github.com/$script:Repository/releases/download/$Tag/$($Release.assets[0].name)"
    return $Release
}
function Asset([int]$Revision, [string]$Game, [string]$Metadata, [string]$Digest) {
    $name = "DofusMiniatures-r$Revision-$Game-$Metadata.zip"
    return [pscustomobject]@{
        draft = $false; prerelease = $false; tag_name = "r$Revision"
        assets = @([pscustomobject]@{
            name = $name; state = 'uploaded'; size = 100
            browser_download_url = "https://github.com/$script:Repository/releases/download/r$Revision/$name"
            digest = "sha256:$Digest"
        })
    }
}
try {
    $gameHash = 'a' * 64
    $metadataHash = 'b' * 64
    $digest = 'c' * 64
    $first = Asset 75 $gameHash $metadataHash $digest
    $otherGame = Asset 79 ('d' * 64) $metadataHash $digest
    $otherMetadata = Asset 80 $gameHash ('d' * 64) $digest
    $draft = Asset 81 $gameHash $metadataHash $digest; $draft.draft = $true
    $preview = Asset 82 $gameHash $metadataHash $digest; $preview.prerelease = $true
    $latest = Asset 78 $gameHash $metadataHash $digest
    Check ((Select-Update @($otherGame, $otherMetadata, $draft, $preview, $latest, $first) $gameHash $metadataHash).revision -eq 78) 'Wrong compatible release selected'
    $external = Asset 90 $gameHash $metadataHash $digest
    $external.assets[0].browser_download_url = 'https://untrusted.example/archive.zip'
    Check ($null -eq (Select-Update @($external) $gameHash $metadataHash)) 'External URL accepted'
    # Renamed repository: assets listed under dofus-enhanced stay trusted.
    $renamed = Asset 91 $gameHash $metadataHash $digest
    $renamed.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-enhanced/releases/download/r91/$($renamed.assets[0].name)"
    $chosen = Select-Update @($renamed) $gameHash $metadataHash
    Check ($null -ne $chosen -and $chosen.revision -eq 91 -and $chosen.url -ceq $renamed.assets[0].browser_download_url) 'Renamed repository asset rejected'
    $legacy = Asset 94 $gameHash $metadataHash $digest
    $legacy.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-miniatures/releases/download/r94/$($legacy.assets[0].name)"
    $chosen = Select-Update @($legacy) $gameHash $metadataHash
    Check ($null -ne $chosen -and $chosen.revision -eq 94 -and $chosen.url -ceq $legacy.assets[0].browser_download_url) 'Former repository asset rejected'
    $other = Asset 92 $gameHash $metadataHash $digest
    $other.assets[0].browser_download_url = "https://github.com/Altansar69/other-repo/releases/download/r92/$($other.assets[0].name)"
    Check ($null -eq (Select-Update @($other) $gameHash $metadataHash)) 'Other repository asset accepted'
    $wrongTag = Asset 93 $gameHash $metadataHash $digest
    $wrongTag.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-enhanced/releases/download/r90/$($wrongTag.assets[0].name)"
    Check ($null -eq (Select-Update @($wrongTag) $gameHash $metadataHash)) 'Renamed asset under another tag accepted'
    # Version tags (1.13.1 on): the archive still names its revision, and its
    # download address sits under the release's own tag.
    $tagged = Asset 95 $gameHash $metadataHash $digest
    $tagged.tag_name = 'v1.3.0'
    $tagged.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-enhanced/releases/download/v1.3.0/$($tagged.assets[0].name)"
    $chosen = Select-Update @($legacy, $tagged) $gameHash $metadataHash
    Check ($null -ne $chosen -and $chosen.revision -eq 95 -and $chosen.url -ceq $tagged.assets[0].browser_download_url) 'Version-tagged release rejected'
    $bare = Asset 96 $gameHash $metadataHash $digest
    $bare.tag_name = '1.3.1'
    $bare.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-miniatures/releases/download/1.3.1/$($bare.assets[0].name)"
    Check ((Select-Update @($bare) $gameHash $metadataHash).revision -eq 96) 'Version tag without its v rejected'
    $newer = Asset 97 $gameHash $metadataHash $digest
    Check ((Select-Update @($tagged, $newer, $bare) $gameHash $metadataHash).revision -eq 97) 'Highest revision not chosen across tag styles'
    $older = Asset 94 $gameHash $metadataHash $digest
    Check ((Select-Update @($older, $tagged) $gameHash $metadataHash).revision -eq 95) 'Older revision tag preferred over a newer version tag'
    $revisionAddress = Asset 98 $gameHash $metadataHash $digest
    $revisionAddress.tag_name = 'v1.4.0'
    Check ($null -eq (Select-Update @($revisionAddress) $gameHash $metadataHash)) 'Version-tagged release downloaded from another tag'
    foreach ($tag in @('v1.4', 'v1.4.0-beta', 'v01.4.0', 'V1.4.0', 'release-1.4.0', 'v1.4.0.1', 'v12345.0.0', 'r98.1', 'r098', '')) {
        $odd = Asset 98 $gameHash $metadataHash $digest
        $odd.tag_name = $tag
        $odd.assets[0].browser_download_url = "https://github.com/Altansar69/dofus-enhanced/releases/download/$tag/$($odd.assets[0].name)"
        Check ($null -eq (Select-Update @($odd) $gameHash $metadataHash)) "Malformed tag '$tag' accepted"
    }
    $mislabeled = Retag (Asset 93 $gameHash $metadataHash $digest) 'r90'
    Check ($null -eq (Select-Update @($mislabeled) $gameHash $metadataHash)) 'Revision tag holding another revision accepted'
    # A version tag lifts none of the other rules.
    $control = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'
    Check ((Select-Update @($control) $gameHash $metadataHash).revision -eq 98) 'Version-tagged control release rejected'
    $draftTagged = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'; $draftTagged.draft = $true
    $previewTagged = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'; $previewTagged.prerelease = $true
    $foreign = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'
    $foreign.assets[0].browser_download_url = "https://github.com/Altansar69/other-repo/releases/download/v1.4.0/$($foreign.assets[0].name)"
    $otherClient = Retag (Asset 98 ('d' * 64) $metadataHash $digest) 'v1.4.0'
    $otherData = Retag (Asset 98 $gameHash ('d' * 64) $digest) 'v1.4.0'
    $undigested = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'; $undigested.assets[0].digest = $null
    $oversized = Retag (Asset 98 $gameHash $metadataHash $digest) 'v1.4.0'; $oversized.assets[0].size = 33MB
    $ancient = Retag (Asset 74 $gameHash $metadataHash $digest) 'v0.74.0'
    $cases = [ordered]@{ draft = $draftTagged; prerelease = $previewTagged; 'other repository' = $foreign; 'other client' = $otherClient
        'other metadata' = $otherData; 'missing digest' = $undigested; oversized = $oversized; 'revision before 75' = $ancient }
    foreach ($case in $cases.GetEnumerator()) {
        Check ($null -eq (Select-Update @($case.Value) $gameHash $metadataHash)) "Version tag bypassed the $($case.Key) rule"
    }
    $first.assets[0].digest = $null
    Check ($null -eq (Select-Update @($first) $gameHash $metadataHash)) 'Missing GitHub digest accepted'
    $first.assets[0].digest = "sha256:$digest"; $first.assets[0].size = 33MB
    Check ($null -eq (Select-Update @($first) $gameHash $metadataHash)) 'Oversized archive accepted'
    $root = Join-Path $fixture 'installed'
    $stage = Join-Path $fixture 'stage'
    $null = [IO.Directory]::CreateDirectory($root)
    $null = [IO.Directory]::CreateDirectory($stage)
    Write-Text (Join-Path $root 'DofusMiniatures-3.7.11.11-settings.ini') "githubUpdates=0`nmode=1`n"
    $settingsHash = (Get-FileHash -LiteralPath (Join-Path $root 'DofusMiniatures-3.7.11.11-settings.ini')).Hash
    $archive = Make-Package 'valid'
    $hash = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
    Reject { Install-Archive $archive $stage $root 75 $gameHash $metadataHash ('0' * 64) } 'Bad GitHub digest accepted'
    Reject { Install-Archive $archive $stage $root 75 $gameHash ('d' * 64) $hash } 'Wrong client metadata accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $root "$script:Family`75"))) 'Failed extraction published a partial release'
    # A fresh stage is required after a deliberately rejected archive.
    $stage = Join-Path $fixture 'stage2'; $null = [IO.Directory]::CreateDirectory($stage)
    Check ((Install-Archive $archive $stage $root 75 $gameHash $metadataHash $hash) -eq 'INSTALLED') 'Verified release was not installed'
    Check ((Install-Archive $archive $stage $root 75 $gameHash $metadataHash $hash) -eq 'ALREADY_INSTALLED') 'Second check must be idempotent'
    Check ((Get-FileHash -LiteralPath (Join-Path $root 'DofusMiniatures-3.7.11.11-settings.ini')).Hash -eq $settingsHash) 'Preferences changed'
    $installed = Join-Path $root "$script:Family`75"
    Write-Text (Join-Path $installed 'TreasureOverlay.exe') 'tampered'
    Reject { Test-Package $installed 75 $gameHash $metadataHash } 'Tampered companion accepted'
    Reject { Install-Archive $archive $stage $root 75 $gameHash $metadataHash $hash } 'Existing corrupt release silently overwritten'
    $badZip = Make-Package 'traversal' 76 '../outside.exe'
    $badHash = (Get-FileHash -LiteralPath $badZip).Hash.ToLowerInvariant()
    Reject { Install-Archive $badZip $stage $root 76 $gameHash $metadataHash $badHash } 'Archive traversal accepted'
    $duplicate = Make-Package 'duplicate' 77 "$script:Family`77/ModuleLoader.exe"
    $badHash = (Get-FileHash -LiteralPath $duplicate).Hash.ToLowerInvariant()
    Reject { Install-Archive $duplicate $stage $root 77 $gameHash $metadataHash $badHash } 'Duplicate path accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $fixture 'outside.exe'))) 'Archive wrote outside staging'

    # Exercise the real orchestration with synthetic installed client files and
    # a deterministic HTTP boundary. No network, registry or game process.
    $client = Join-Path $fixture 'client'
    $metaDirectory = Join-Path $client 'Dofus_Data/il2cpp_data/Metadata'
    $null = [IO.Directory]::CreateDirectory($metaDirectory)
    Write-Text (Join-Path $client 'Dofus.exe') 'fake'
    Write-Text (Join-Path $client 'GameAssembly.dll') 'future game'
    Write-Text (Join-Path $metaDirectory 'global-metadata.dat') 'future metadata'
    $gameHash = (Get-FileHash -LiteralPath (Join-Path $client 'GameAssembly.dll')).Hash.ToLowerInvariant()
    $metadataHash = (Get-FileHash -LiteralPath (Join-Path $metaDirectory 'global-metadata.dat')).Hash.ToLowerInvariant()
    function Set-Game([string]$Label) {
        Write-Text (Join-Path $client 'GameAssembly.dll') "game $Label"
        Write-Text (Join-Path $metaDirectory 'global-metadata.dat') "metadata $Label"
        $script:gameHash = (Get-FileHash -LiteralPath (Join-Path $client 'GameAssembly.dll')).Hash.ToLowerInvariant()
        $script:metadataHash = (Get-FileHash -LiteralPath (Join-Path $metaDirectory 'global-metadata.dat')).Hash.ToLowerInvariant()
    }
    $forkName = 'vDAKK/dofus-enhanced'
    $exe = Join-Path $client 'Dofus.exe'
    function Source-Release([int]$Revision, [string]$Repository, [string]$Archive) {
        $release = Asset $Revision $gameHash $metadataHash (Get-FileHash -LiteralPath $Archive).Hash.ToLowerInvariant()
        $release.assets[0].size = (Get-Item -LiteralPath $Archive).Length
        $release.assets[0].browser_download_url = "https://github.com/$Repository/releases/download/r$Revision/$($release.assets[0].name)"
        return $release
    }
    function Get-TrustedDownload([string]$Url, [string]$Destination, [long]$Limit, [Threading.CancellationToken]$Cancellation) {
        $script:requested += $Url
        if ($Url.StartsWith("https://api.github.com/repos/$script:Repository/")) {
            if ($script:primaryOffline) { throw 'simulated offline primary' }
            Write-Text $Destination (ConvertTo-Json -InputObject @($script:primaryReleases) -Depth 6)
        } elseif ($Url.StartsWith("https://api.github.com/repos/$forkName/")) {
            if ($script:forkOffline) { throw 'simulated offline fork' }
            if ($Url.EndsWith('/releases/latest')) { Write-Text $Destination (ConvertTo-Json -InputObject $script:forkLatest -Depth 6) }
            elseif ($script:forkEmpty) { Write-Text $Destination '[]' }
            else { Write-Text $Destination (ConvertTo-Json -InputObject @($script:forkReleases) -Depth 6) }
        } else { [IO.File]::Copy($script:archive, $Destination) }
    }
    function Run-Update { $script:requested = @(); return @(Invoke-GitHubUpdate $root $exe) }
    $listingsOf = { @($script:requested | Where-Object { $_.StartsWith('https://api.github.com/') }).Count }
    $script:primaryOffline = $false; $script:forkOffline = $false; $script:forkEmpty = $false

    # A game update DAKK has not ported yet: Altansar's repository rescues it.
    Set-Game 'future'
    $unrelated = Asset 77 ('d' * 64) $metadataHash $digest
    $script:archive = Make-Package 'rescue78' 78
    $script:primaryReleases = @(Source-Release 78 $script:Repository $script:archive)
    $script:forkReleases = @($unrelated)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_INSTALLED r78' -and $script:requested.Count -eq 3 -and
        $script:requested[0].StartsWith("https://api.github.com/repos/$forkName/") -and
        $script:requested[2].StartsWith("https://github.com/$script:Repository/")) 'Altansar did not rescue an unported game update'
    Check ((Get-FileHash -LiteralPath (Join-Path $root 'DofusMiniatures-3.7.11.11-settings.ini')).Hash -eq $settingsHash) 'Preferences changed by the rescue'
    # A working release is never replaced by Altansar's, whatever its number.
    $script:primaryReleases = @(Source-Release 90 $script:Repository $script:archive)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LOCAL_NEWER r78' -and $script:requested.Count -eq 1 -and
        !(Test-Path -LiteralPath (Join-Path $root "$script:Family`90"))) 'Altansar replaced a working release'
    # A degraded GitHub answers the fork's listing with an empty page: the newest
    # release endpoint still finds the compatible package.
    $script:archive = Make-Package 'newest79' 79
    $script:forkLatest = Source-Release 79 $forkName $script:archive
    $script:forkEmpty = $true
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LISTING_EMPTY|UPDATE_INSTALLED r79' -and $script:requested.Count -eq 3 -and
        $script:requested[1] -ceq "https://api.github.com/repos/$forkName/releases/latest") 'Empty listing must fall back to the newest release'
    $script:forkLatest = $unrelated
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LISTING_EMPTY|UPDATE_LOCAL_NEWER r79' -and $script:requested.Count -eq 2) 'An incompatible newest release must not be downloaded'
    # Releases without a compatible one are a definitive answer: no second request.
    $script:forkEmpty = $false
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_LOCAL_NEWER r79' -and $script:requested.Count -eq 1) 'A listing without a compatible release must not query the newest release'
    # The fork's own newer release installs; its current one fetches nothing.
    $script:archive = Make-Package 'fork81' 81
    $script:forkReleases = @(Source-Release 81 $forkName $script:archive)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_INSTALLED r81' -and $script:requested[-1].StartsWith("https://github.com/$forkName/")) 'The fork release was not installed'
    Test-Package (Join-Path $root "$script:Family`81") 81 $gameHash $metadataHash
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_ALREADY_INSTALLED r81' -and $script:requested.Count -eq 1) 'Unnecessary repeated archive download'
    # Offline with a working release: reported, kept, Altansar not asked.
    $script:forkOffline = $true; $script:requested = @()
    Reject { Invoke-GitHubUpdate $root $exe } 'A silent fork must be reported'
    Check ($script:requested.Count -eq 1) 'Altansar was asked while a release works'
    Test-Package (Join-Path $root "$script:Family`81") 81 $gameHash $metadataHash
    $script:forkOffline = $false
    # The fork's archive gets the same checks: a wrong digest installs nothing.
    $script:archive = Make-Package 'fork86' 86
    $tampered = Source-Release 86 $forkName $script:archive
    $tampered.assets[0].digest = 'sha256:' + ('0' * 64)
    $script:forkReleases = @($tampered)
    Reject { Invoke-GitHubUpdate $root $exe } 'A corrupt fork archive was accepted'
    Check (!(Test-Path -LiteralPath (Join-Path $root "$script:Family`86"))) 'A corrupt fork archive left a release'
    Check (@(Get-ChildItem -LiteralPath $root -Force -Directory | Where-Object Name -Like '.dofus-update-*').Count -eq 0) 'Staging cleanup failed'

    # Another game update: the fork still comes first when both have a port.
    $savedGame = $gameHash; $savedMetadata = $metadataHash
    Set-Game 'next'
    $script:archive = Make-Package 'fork91' 91
    $script:forkReleases = @(Source-Release 91 $forkName $script:archive)
    $script:primaryReleases = @(Source-Release 92 $script:Repository $script:archive)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_INSTALLED r91' -and (& $listingsOf) -eq 1) 'The fork must win without asking Altansar'
    # A listing cannot designate another source's asset, in either direction.
    Set-Game 'third'
    $script:archive = Make-Package 'crossed84' 84
    $script:primaryReleases = @(Source-Release 84 $forkName $script:archive)
    $script:forkReleases = @(Source-Release 84 $script:Repository $script:archive)
    $result = Run-Update
    Check (($result -join '|') -ceq 'UPDATE_NO_COMPATIBLE_RELEASE' -and !(Test-Path -LiteralPath (Join-Path $root "$script:Family`84"))) 'An asset of another source was accepted'
    # A silent fork does not hide the rescue; the failure stays visible.
    $script:archive = Make-Package 'rescue93' 93
    $script:primaryReleases = @(Source-Release 93 $script:Repository $script:archive)
    $script:forkOffline = $true
    $result = Run-Update
    Check ($result[-1] -ceq 'UPDATE_INSTALLED r93' -and $result[0] -clike "UPDATE_SOURCE_UNAVAILABLE $forkName *") 'An offline fork hid the rescue'
    # A silent rescue changes nothing when the fork answered.
    Set-Game 'fourth'
    $script:forkOffline = $false; $script:primaryOffline = $true
    $script:forkReleases = @($unrelated)
    $result = Run-Update
    Check ($result[-1] -ceq 'UPDATE_NO_COMPATIBLE_RELEASE' -and $result[0] -clike "UPDATE_SOURCE_UNAVAILABLE $script:Repository *") 'An offline rescue made an answered check fail'
    # Offline on both: reported, every local release kept.
    $script:forkOffline = $true
    Reject { Invoke-GitHubUpdate $root $exe } 'Both sources offline must be reported'
    Test-Package (Join-Path $root "$script:Family`81") 81 $savedGame $savedMetadata
    Check (@(Get-ChildItem -LiteralPath $root -Force -Directory | Where-Object Name -Like '.dofus-update-*').Count -eq 0) 'Staging cleanup failed after the source checks'
    Write-Output 'PASS: stable compatible selection, version and revision tags, trusted assets, atomic install, no overwrite, no traversal, integrity, idempotence, future client, empty-listing fallback to the newest release, renamed repository assets, fork first, Altansar only as a rescue, assets scoped to their source, offline retention; fixtures only'
} finally {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($fixture)) -ne [IO.Path]::GetFullPath($build) -or
        [IO.Path]::GetFileName($fixture) -notmatch '^update-fixtures-[a-f0-9]{32}$') { throw 'Invalid fixture cleanup target' }
    Assert-PlainPath $fixture $true
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
