param(
    [string]$InstallRoot,
    [string]$ClientExe,
    [switch]$LibraryOnly
)
# Windows PowerShell 5.1; no module installation, credentials or game interaction.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# A launcher may inherit PSModulePath from PowerShell 7 or a build environment.
# Resolve the installed Windows module explicitly instead of depending on it.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$script:Repository = 'vDAKK/better-dofus'
$script:Family = 'BetterDofus-'
$script:PayloadFiles = @('BetterDofus.dll', 'BetterDofusLoader.exe', 'BetterDofusAutoLoader.exe',
    'BetterDofusTreasure.exe', 'Update-GitHub.ps1')

# X.Y.Z as one comparable number, as in release.ini; 0 for anything else.
function Get-Revision([string]$Version) {
    if ($Version -cnotmatch '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$') { return 0 }
    return [int]$Matches[1] * 1000000 + [int]$Matches[2] * 1000 + [int]$Matches[3]
}

function Assert-PlainPath([string]$Path, [bool]$Directory) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.PSIsContainer -ne $Directory) {
        throw 'Expected a plain file or directory'
    }
}

function Read-Fields([string]$Path) {
    Assert-PlainPath $Path $false
    if ((Get-Item -LiteralPath $Path).Length -gt 4096) { throw 'Manifest too large' }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if (!$line) { continue }
        $pair = $line -split '=', 2
        if ($pair.Count -ne 2 -or !$pair[0] -or !$pair[1] -or $fields.ContainsKey($pair[0])) {
            throw 'Invalid or duplicate manifest field'
        }
        $fields.Add($pair[0], $pair[1])
    }
    return $fields
}

function Test-Package([string]$Folder, [int]$Revision, [string]$GameHash, [string]$MetadataHash) {
    Assert-PlainPath $Folder $true
    $manifest = Read-Fields (Join-Path $Folder 'release.ini')
    if ($manifest.Count -ne 8 -or $manifest.format -cne '1' -or
        $manifest.revision -cne [string]$Revision -or
        $manifest.clientVersion -cnotmatch '^[0-9]{1,6}(\.[0-9]{1,6}){3}$' -or
        $manifest.gameHash -cne $GameHash -or $manifest.metadataHash -cne $MetadataHash) {
        throw 'Package is incompatible with the installed Dofus files'
    }
    $hashes = Read-Fields (Join-Path $Folder 'update-files.ini')
    if ($hashes.Count -ne $script:PayloadFiles.Count) { throw 'Invalid integrity manifest' }
    foreach ($name in $script:PayloadFiles) {
        if (!$hashes.ContainsKey($name) -or $hashes[$name] -cnotmatch '^[a-f0-9]{64}$') {
            throw 'Missing integrity fingerprint'
        }
        $path = Join-Path $Folder $name
        Assert-PlainPath $path $false
        if ((Get-Item -LiteralPath $path).Length -gt 64MB -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$name]) {
            throw 'Package file integrity check failed'
        }
        if ($name -in @('BetterDofus.dll', 'BetterDofusLoader.exe', 'BetterDofusAutoLoader.exe') -and
            $manifest[$name] -cne $hashes[$name]) { throw 'Conflicting integrity manifests' }
    }
}

# A release is v<X.Y.Z> with the one asset BetterDofus-<X.Y.Z>.zip. The Dofus build
# it supports is a comment of its notes, invisible on the page, so that nothing is
# downloaded to know it; the unpacked release.ini must then say the same.
function Select-Update($Releases, [string]$GameHash, [string]$MetadataHash) {
    $selected = $null
    foreach ($release in @($Releases)) {
        $tag = [string]$release.tag_name
        if ($release.draft -or $release.prerelease -or $tag -cnotmatch '^v(.+)$') { continue }
        $version = $Matches[1]
        $revision = Get-Revision $version
        if ($revision -lt 2000000) { continue }
        $body = if ($release.PSObject.Properties['body']) { [string]$release.body } else { '' }
        if ($body -cnotmatch '<!-- better-dofus game=([a-f0-9]{64}) metadata=([a-f0-9]{64}) -->' -or
            $Matches[1] -cne $GameHash -or $Matches[2] -cne $MetadataHash) { continue }
        foreach ($asset in @($release.assets)) {
            if ($asset.name -cne "BetterDofus-$version.zip") { continue }
            if ($asset.state -cne 'uploaded' -or $asset.size -le 0 -or $asset.size -gt 32MB) { continue }
            # GitHub computes the digest on upload; never trust a checksum from
            # an arbitrary asset URL or silently accept an absent digest.
            if (!$asset.PSObject.Properties['digest'] -or $asset.digest -cnotmatch '^sha256:[a-f0-9]{64}$') { continue }
            $expected = "https://github.com/$script:Repository/releases/download/$tag/$($asset.name)"
            if ($asset.browser_download_url -cne $expected) { continue }
            if (!$selected -or $revision -gt $selected.revision) {
                $selected = [pscustomobject]@{
                    revision = $revision; version = $version; url = $expected; size = [long]$asset.size
                    hash = $asset.digest.Substring(7)
                }
            }
        }
    }
    return $selected
}

function Get-TrustedDownload([string]$Url, [string]$Destination, [long]$Limit,
    [Threading.CancellationToken]$Cancellation) {
    Add-Type -AssemblyName System.Net.Http
    $handler = New-Object Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $handler.UseCookies = $false
    $handler.UseDefaultCredentials = $false
    $client = New-Object Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(30)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('BetterDofus-Updater/1')
    $client.DefaultRequestHeaders.Accept.ParseAdd('application/vnd.github+json')
    $client.DefaultRequestHeaders.Add('X-GitHub-Api-Version', '2022-11-28')
    try {
        $address = [Uri]$Url
        for ($redirect = 0; $redirect -le 5; $redirect++) {
            if ($address.Scheme -ne 'https' -or $address.Port -ne 443 -or $address.UserInfo -or
                $address.Host -notin @('api.github.com', 'github.com',
                    'release-assets.githubusercontent.com', 'objects.githubusercontent.com')) {
                throw 'Untrusted download destination'
            }
            $message = New-Object Net.Http.HttpRequestMessage([Net.Http.HttpMethod]::Get, $address)
            $response = $null
            try {
                $response = $client.SendAsync($message, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $Cancellation).GetAwaiter().GetResult()
                if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
                    if (!$response.Headers.Location) { throw 'Redirect without a location' }
                    $address = New-Object Uri($address, $response.Headers.Location)
                    continue
                }
                if (!$response.IsSuccessStatusCode) { throw "GitHub HTTP $([int]$response.StatusCode)" }
                if ($response.Content.Headers.ContentLength -gt $Limit) { throw 'Download too large' }
                $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $output = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try {
                    $buffer = New-Object byte[] 65536
                    [long]$total = 0
                    while (($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $Cancellation).GetAwaiter().GetResult()) -gt 0) {
                        $total += $count
                        if ($total -gt $Limit) { throw 'Download exceeds its bound' }
                        $output.Write($buffer, 0, $count)
                    }
                } finally { $output.Dispose(); $stream.Dispose() }
                return
            } finally {
                if ($response) { $response.Dispose() }
                $message.Dispose()
            }
        }
        throw 'Too many redirects'
    } finally { $client.Dispose(); $handler.Dispose() }
}

function Install-Archive([string]$Archive, [string]$Stage, [string]$Root,
    [string]$Version, [string]$GameHash, [string]$MetadataHash, [string]$ExpectedHash) {
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ExpectedHash) {
        throw 'GitHub asset digest mismatch'
    }
    Assert-PlainPath $Root $true
    Assert-PlainPath $Stage $true
    $revision = Get-Revision $Version
    if (!$revision) { throw 'Invalid version' }
    $name = "$script:Family$Version"
    $destination = Join-Path $Root $name
    if (Test-Path -LiteralPath $destination) {
        Test-Package $destination $revision $GameHash $MetadataHash
        return 'ALREADY_INSTALLED'
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        # The update archive contains only runtime files, no user preferences,
        # source snapshots, game files, executable setup hooks or arbitrary paths.
        $allowed = @($script:PayloadFiles) + @('release.ini', 'update-files.ini', 'LISEZ-MOI.txt', 'WEBVIEW2-LICENSE.txt')
        if ($zip.Entries.Count -ne $allowed.Count) { throw 'Unexpected archive contents' }
        $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
        [long]$total = 0
        foreach ($entry in $zip.Entries) {
            $prefix = "$name/"
            if (!$entry.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Invalid archive root' }
            $leaf = $entry.FullName.Substring($prefix.Length)
            if ($leaf -cnotin $allowed -or !$seen.Add($leaf)) { throw 'Unexpected or duplicate archive path' }
            # Refuse Unix symlinks and DOS reparse points, even in a ZIP.
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or
                ($entry.ExternalAttributes -band 0x400)) { throw 'Archive link forbidden' }
            $total += $entry.Length
            if ($entry.Length -lt 1 -or $entry.Length -gt 64MB -or $total -gt 128MB) { throw 'Archive expansion bound exceeded' }
        }
        $unpacked = Join-Path $Stage $name
        $null = [IO.Directory]::CreateDirectory($unpacked)
        foreach ($entry in $zip.Entries) {
            $file = Join-Path $unpacked $entry.Name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $file, $false)
        }
    } finally { $zip.Dispose() }
    Test-Package $unpacked $revision $GameHash $MetadataHash
    # A complete verified release becomes visible in a single directory rename.
    # Existing releases and shared settings are never overwritten.
    [IO.Directory]::Move($unpacked, $destination)
    return 'INSTALLED'
}

function Find-Update([string]$Folder, [string]$GameHash, [string]$MetadataHash,
    [Threading.CancellationToken]$Cancellation) {
    $listing = Join-Path $Folder 'releases.json'
    Get-TrustedDownload "https://api.github.com/repos/$script:Repository/releases?per_page=100" $listing 4MB $Cancellation
    $releases = [IO.File]::ReadAllText($listing) | ConvertFrom-Json
    if (@($releases).Count -ne 0) { return Select-Update $releases $GameHash $MetadataHash }
    # A degraded GitHub can answer the listing with an empty page (seen on
    # 2026-10-22); the newest published release has its own endpoint. A listing
    # with releases but none compatible stays a definitive answer.
    Write-Output 'UPDATE_LISTING_EMPTY'
    $newest = Join-Path $Folder 'latest.json'
    Get-TrustedDownload "https://api.github.com/repos/$script:Repository/releases/latest" $newest 1MB $Cancellation
    return Select-Update ([IO.File]::ReadAllText($newest) | ConvertFrom-Json) $GameHash $MetadataHash
}

function Invoke-GitHubUpdate([string]$Root, [string]$Exe) {
    if (![IO.Path]::IsPathRooted($Root) -or ![IO.Path]::IsPathRooted($Exe) -or
        [IO.Path]::GetFileName($Exe) -ine 'Dofus.exe') { throw 'Absolute installation and client paths required' }
    $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    Assert-PlainPath $Root $true
    Assert-PlainPath $Exe $false
    $parent = [IO.Path]::GetDirectoryName($Exe)
    $gameHash = (Get-FileHash -LiteralPath (Join-Path $parent 'GameAssembly.dll')).Hash.ToLowerInvariant()
    $metadataHash = (Get-FileHash -LiteralPath (Join-Path $parent 'Dofus_Data/il2cpp_data/Metadata/global-metadata.dat')).Hash.ToLowerInvariant()
    $stage = Join-Path $Root ('.better-dofus-update-' + [Guid]::NewGuid().ToString('N'))
    $null = [IO.Directory]::CreateDirectory($stage)
    $cancellation = New-Object Threading.CancellationTokenSource
    # The loader kills this worker after 45 seconds.
    $cancellation.CancelAfter(40000)
    try {
        # Only a release that verifies against the installed Dofus counts: an
        # incompatible sibling is not a fallback.
        $localRevision = 0
        $localVersion = ''
        foreach ($directory in Get-ChildItem -LiteralPath $Root -Directory) {
            if ($directory.Name.StartsWith($script:Family, [StringComparison]::Ordinal)) {
                $version = $directory.Name.Substring($script:Family.Length)
                $revision = Get-Revision $version
                if ($revision -gt $localRevision) {
                    try {
                        Test-Package $directory.FullName $revision $gameHash $metadataHash
                        $localRevision = $revision
                        $localVersion = $version
                    } catch { }
                }
            }
        }
        $update = $null
        foreach ($entry in @(Find-Update $stage $gameHash $metadataHash $cancellation.Token)) {
            if ($entry -is [string]) { Write-Output $entry } elseif ($entry) { $update = $entry }
        }
        if (!$update -and !$localRevision) { Write-Output 'UPDATE_NO_COMPATIBLE_RELEASE'; return }
        if (!$update -or $update.revision -lt $localRevision) { Write-Output "UPDATE_LOCAL_NEWER $localVersion"; return }
        if ($update.revision -eq $localRevision) { Write-Output "UPDATE_ALREADY_INSTALLED $localVersion"; return }
        $archive = Join-Path $stage 'package.zip'
        Get-TrustedDownload $update.url $archive $update.size $cancellation.Token
        if ((Get-Item -LiteralPath $archive).Length -ne $update.size) { throw 'Truncated asset' }
        $result = Install-Archive $archive $stage $Root $update.version $gameHash $metadataHash $update.hash
        Write-Output "UPDATE_$result $($update.version)"
    } finally {
        $cancellation.Dispose()
        # This exact freshly created staging directory is the only recursive
        # deletion target. Reject a changed parent or a substituted junction.
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($stage)) -ne $Root -or
            [IO.Path]::GetFileName($stage) -cnotmatch '^\.better-dofus-update-[a-f0-9]{32}$') {
            throw 'Invalid staging cleanup target'
        }
        Assert-PlainPath $stage $true
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}

if (!$LibraryOnly) {
    try { Invoke-GitHubUpdate $InstallRoot $ClientExe; exit 0 }
    catch { Write-Output "UPDATE_UNAVAILABLE $($_.Exception.Message)"; exit 1 }
}
