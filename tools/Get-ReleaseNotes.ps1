param(
    [Parameter(Mandatory)][ValidatePattern('^v\d+\.\d+\.\d+(-preview\.\d+)?$')][string]$Tag,
    [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$version = $Tag.Substring(1)
# Keep compatibility with the heading of the first published preview.
if ($Tag -eq 'v0.2.0-preview.1') { $version = '0.2.0 Preview' }
$source = Join-Path $PSScriptRoot '../docs/RELEASE_NOTES.md'
$content = [System.IO.File]::ReadAllText($source)
$pattern = '(?m)^## FluxTop ' + [regex]::Escape($version) + '(?=\s|—|$)'
$match = [regex]::Match($content, $pattern)
if (-not $match.Success) {
    throw "Missing matching heading '## FluxTop $version' in docs/RELEASE_NOTES.md"
}
$section = $content.Substring($match.Index)
$next = [regex]::Match($section.Substring($match.Length), '(?m)^## FluxTop \d+\.\d+\.\d+')
if ($next.Success) { $section = $section.Substring(0, $match.Length + $next.Index) }
$section = [regex]::Replace($section, '(?m)^# История выпусков FluxTop[^\r\n]*\r?\n', '')
$section = [regex]::Replace($section, '(?ms)^### Проверка перед публикацией[^\r\n]*\r?\n.*\z', '')
$section = [regex]::Replace($section, '(?m)^\*\*Важно:\*\* опубликованный v0\.2\.1[^\r\n]*\r?\n', '')
$section = $section.Trim() + "`n"
[System.IO.File]::WriteAllText($OutputPath, $section, [System.Text.UTF8Encoding]::new($false))