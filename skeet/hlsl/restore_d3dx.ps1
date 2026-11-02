$ErrorActionPreference = 'Stop'
$packageBase = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\packages'))
$packagePath = Join-Path $packageBase 'Microsoft.DXSDK.D3DX.9.29.952.8'
if (Test-Path -LiteralPath (Join-Path $packagePath 'build\native\include\d3dx9effect.h')) { return }
[IO.Directory]::CreateDirectory($packagePath) | Out-Null
$packageArchive = Join-Path $packageBase 'microsoft.dxsdk.d3dx.9.29.952.8.nupkg'
if (!(Test-Path -LiteralPath $packageArchive)) {
    Invoke-WebRequest -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.dxsdk.d3dx/9.29.952.8/microsoft.dxsdk.d3dx.9.29.952.8.nupkg' -OutFile $packageArchive
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packageArchive)
try {
    foreach ($entry in $archive.Entries) {
        if ($entry.FullName -notmatch '^build/native/include/[^/]+\.(h|inl)$|^LICENSE\.txt$') { continue }
        $target = [IO.Path]::GetFullPath((Join-Path $packagePath $entry.FullName))
        if (!$target.StartsWith($packagePath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package path' }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        $inputStream = $entry.Open()
        try { $outputStream = [IO.File]::Create($target); try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() } } finally { $inputStream.Dispose() }
    }
} finally { $archive.Dispose() }
Write-Output 'Restored official Microsoft.DXSDK.D3DX 9.29.952.8 headers. Runtime DLLs are not copied.'
