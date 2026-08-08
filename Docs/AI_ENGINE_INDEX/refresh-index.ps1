[CmdletBinding()]
param(
    [string]$RepositoryRoot
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}

$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$outputDirectory = Join-Path $root "Docs\AI_ENGINE_INDEX"
$findingsPath = Join-Path $outputDirectory "findings.jsonl"
$metadataPath = Join-Path $outputDirectory "review-metadata.json"
$manifestPath = Join-Path $outputDirectory "source-manifest.jsonl"
$summaryPath = Join-Path $outputDirectory "manifest-summary.json"
$indexPath = Join-Path $outputDirectory "index.json"
$humanReportPath = Join-Path $outputDirectory "Voxagine_Engine_Review.docx"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Convert-ToRelativePath {
    param([string]$Path)

    $rootUri = New-Object System.Uri(($root.TrimEnd("\") + "\"))
    $pathUri = New-Object System.Uri($Path)
    return [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString()).Replace("\", "/")
}

function Get-Subsystem {
    param([string]$RelativePath)

    switch -Regex ($RelativePath) {
        '^Voxagine/Source/Core/Application\.' { return 'application' }
        '^Voxagine/Source/Core/JsonSerializer\.' { return 'serialization' }
        '^Voxagine/Source/Core/ECS/Systems/Chunk/' { return 'chunks' }
        '^Voxagine/Source/Core/ECS/Systems/Pathfinding/' { return 'pathfinding' }
        '^Voxagine/Source/Core/ECS/Systems/Physics/' { return 'physics' }
        '^Voxagine/Source/Core/ECS/Systems/Rendering/' { return 'rendering' }
        '^Voxagine/Source/Core/ECS/Systems/Audio/' { return 'audio' }
        '^Voxagine/Source/Core/ECS/' { return 'ecs' }
        '^Voxagine/Source/Core/Platform/Input/' { return 'input' }
        '^Voxagine/Source/Core/Platform/Audio/' { return 'audio' }
        '^Voxagine/Source/Core/Platform/Rendering/' { return 'rendering' }
        '^Voxagine/Source/Core/Platform/' { return 'platform' }
        '^Voxagine/Source/Core/Threading/' { return 'job-system' }
        '^Voxagine/Source/Core/Memory/' { return 'memory' }
        '^Voxagine/Source/Core/Resources/' { return 'resources' }
        '^Voxagine/Source/Core/LoggingSystem/' { return 'logging' }
        '^Voxagine/Source/Core/PlayerPrefs/' { return 'player-prefs' }
        '^Voxagine/Source/Core/System/' { return 'filesystem' }
        '^Voxagine/Source/Core/Utils/' { return 'utilities' }
        '^Voxagine/Source/Core/' { return 'core' }
        '^Voxagine/Source/Editor/' { return 'editor' }
        '^Voxagine/Source/Bringup/' { return 'bringup' }
        '^Game/Source/' { return 'game' }
        '^UnitTesting/' { return 'tests' }
        '^cmake/|^CMake' { return 'build' }
        '^\.github/workflows/' { return 'ci' }
        default { return 'repository' }
    }
}

function Get-TextKind {
    param([string]$Extension)

    switch ($Extension.ToLowerInvariant()) {
        '.h' { return 'cpp-header' }
        '.hpp' { return 'cpp-header' }
        '.inl' { return 'cpp-inline' }
        '.c' { return 'c-source' }
        '.cc' { return 'cpp-source' }
        '.cpp' { return 'cpp-source' }
        '.hlsl' { return 'shader' }
        '.glsl' { return 'shader' }
        '.vert' { return 'shader' }
        '.frag' { return 'shader' }
        '.comp' { return 'shader' }
        '.cmake' { return 'cmake' }
        '.ps1' { return 'powershell' }
        '.json' { return 'json' }
        '.yml' { return 'yaml' }
        '.yaml' { return 'yaml' }
        '.md' { return 'markdown' }
        default { return 'text' }
    }
}

function Get-OrderedCounts {
    param([object[]]$Records, [string]$Property)

    $result = [ordered]@{}
    foreach ($group in ($Records | Group-Object -Property $Property | Sort-Object Name)) {
        $result[$group.Name] = $group.Count
    }
    return $result
}

$scanDirectories = @(
    'Voxagine/Source/Core',
    'Voxagine/Source/Editor',
    'Voxagine/Source/Bringup',
    'Game/Source',
    'UnitTesting/UnitTests/Source',
    'cmake',
    '.github/workflows'
)

$topLevelFiles = @(
    'CMakeLists.txt',
    'CMakePresets.json',
    'CMakeUserPresets.json',
    'README.md',
    'AI_INDEX.md'
)

$allowedExtensions = @(
    '.h', '.hpp', '.inl', '.c', '.cc', '.cpp',
    '.hlsl', '.glsl', '.vert', '.frag', '.comp',
    '.cmake', '.ps1', '.json', '.yml', '.yaml', '.md'
)

$sourceFiles = New-Object System.Collections.Generic.List[System.IO.FileInfo]
foreach ($relativeDirectory in $scanDirectories) {
    $absoluteDirectory = Join-Path $root $relativeDirectory
    if (Test-Path -LiteralPath $absoluteDirectory -PathType Container) {
        foreach ($file in Get-ChildItem -LiteralPath $absoluteDirectory -File -Recurse) {
            if ($allowedExtensions -contains $file.Extension.ToLowerInvariant()) {
                $sourceFiles.Add($file)
            }
        }
    }
}

foreach ($relativeFile in $topLevelFiles) {
    $absoluteFile = Join-Path $root $relativeFile
    if (Test-Path -LiteralPath $absoluteFile -PathType Leaf) {
        $sourceFiles.Add((Get-Item -LiteralPath $absoluteFile))
    }
}

$deduplicated = @{}
foreach ($file in $sourceFiles) {
    $deduplicated[$file.FullName.ToLowerInvariant()] = $file
}

$manifestRecords = New-Object System.Collections.Generic.List[object]
$manifestLines = New-Object System.Collections.Generic.List[string]
foreach ($file in ($deduplicated.Values | Sort-Object FullName)) {
    $relativePath = Convert-ToRelativePath -Path $file.FullName
    $text = [System.IO.File]::ReadAllText($file.FullName)
    if ($text.Length -eq 0) {
        $lineCount = 0
    }
    else {
        $lineCount = ([regex]::Matches($text, "`r`n|`n|`r").Count + 1)
    }

    $symbols = New-Object System.Collections.Generic.List[string]
    $symbolPatterns = @(
        '(?m)^\s*(?:class|struct|union|enum(?:\s+class)?)\s+(?:[A-Z_][A-Z0-9_]*\s+)?([A-Za-z_][A-Za-z0-9_]*)',
        '(?m)^\s*(?:[A-Za-z_][A-Za-z0-9_:<>,~*&\[\]\s]+\s+)?([A-Za-z_~][A-Za-z0-9_~]*(?:::[A-Za-z_~][A-Za-z0-9_~]*)+)\s*\('
    )
    foreach ($pattern in $symbolPatterns) {
        foreach ($match in [regex]::Matches($text, $pattern)) {
            $value = $match.Groups[1].Value
            if (-not [string]::IsNullOrWhiteSpace($value) -and -not $symbols.Contains($value)) {
                $symbols.Add($value)
            }
            if ($symbols.Count -ge 60) { break }
        }
        if ($symbols.Count -ge 60) { break }
    }

    $includes = New-Object System.Collections.Generic.List[string]
    foreach ($match in [regex]::Matches($text, '(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]')) {
        $value = $match.Groups[1].Value.Replace("\", "/")
        if (-not $includes.Contains($value)) {
            $includes.Add($value)
        }
        if ($includes.Count -ge 40) { break }
    }

    $record = [ordered]@{
        schema_version = 1
        path = $relativePath
        subsystem = Get-Subsystem -RelativePath $relativePath
        kind = Get-TextKind -Extension $file.Extension
        lines = $lineCount
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        symbols = @($symbols)
        includes = @($includes)
    }
    $manifestRecords.Add([pscustomobject]$record)
    $manifestLines.Add(($record | ConvertTo-Json -Compress -Depth 6))
}

[System.IO.File]::WriteAllLines($manifestPath, $manifestLines, $utf8NoBom)
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

if (-not (Test-Path -LiteralPath $findingsPath -PathType Leaf)) {
    throw "Missing canonical finding database: $findingsPath"
}

$findingRecords = New-Object System.Collections.Generic.List[object]
$findingIds = @{}
$lineNumber = 0
foreach ($line in [System.IO.File]::ReadAllLines($findingsPath)) {
    $lineNumber++
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    try {
        $finding = $line | ConvertFrom-Json
    }
    catch {
        throw "Invalid findings JSON at line ${lineNumber}: $($_.Exception.Message)"
    }
    if ([string]::IsNullOrWhiteSpace($finding.id)) {
        throw "Finding at line $lineNumber has no id."
    }
    if ($findingIds.ContainsKey($finding.id)) {
        throw "Duplicate finding id '$($finding.id)' at line $lineNumber."
    }
    $findingIds[$finding.id] = $true
    $findingRecords.Add($finding)
}

$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
$safeRoot = $root.Replace("\", "/")
$headRevision = (& git -c "safe.directory=$safeRoot" -C $root rev-parse HEAD).Trim()
$findingsHash = (Get-FileHash -LiteralPath $findingsPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifestRecordArray = @($manifestRecords | ForEach-Object { $_ })
$findingRecordArray = @($findingRecords | ForEach-Object { $_ })

$summary = [ordered]@{
    schema_version = 1
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    file_count = $manifestRecords.Count
    total_lines = ($manifestRecords | Measure-Object -Property lines -Sum).Sum
    total_bytes = ($manifestRecords | Measure-Object -Property bytes -Sum).Sum
    by_subsystem = Get-OrderedCounts -Records $manifestRecordArray -Property 'subsystem'
    by_kind = Get-OrderedCounts -Records $manifestRecordArray -Property 'kind'
    source_manifest_sha256 = $manifestHash
}
[System.IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8), $utf8NoBom)

$humanReport = [ordered]@{
    canonical_path = 'Docs/AI_ENGINE_INDEX/Voxagine_Engine_Review.docx'
    exists = (Test-Path -LiteralPath $humanReportPath -PathType Leaf)
}
if ($humanReport.exists) {
    $humanReport.bytes = (Get-Item -LiteralPath $humanReportPath).Length
    $humanReport.sha256 = (Get-FileHash -LiteralPath $humanReportPath -Algorithm SHA256).Hash.ToLowerInvariant()
}

$index = [ordered]@{
    schema_version = 1
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    repository = [ordered]@{
        root_name = Split-Path -Leaf $root
        head_revision = $headRevision
        review_baseline_revision = $metadata.review.baseline_revision
        baseline_matches_head = ($headRevision -eq $metadata.review.baseline_revision)
    }
    review = $metadata.review
    scope = $metadata.scope
    verification = $metadata.verification
    findings = [ordered]@{
        count = $findingRecords.Count
        by_priority = Get-OrderedCounts -Records $findingRecordArray -Property 'priority'
        by_severity = Get-OrderedCounts -Records $findingRecordArray -Property 'severity'
        by_status = Get-OrderedCounts -Records $findingRecordArray -Property 'status'
        by_subsystem = Get-OrderedCounts -Records $findingRecordArray -Property 'subsystem'
        sha256 = $findingsHash
        canonical_path = 'Docs/AI_ENGINE_INDEX/findings.jsonl'
    }
    sources = [ordered]@{
        count = $manifestRecords.Count
        total_lines = $summary.total_lines
        sha256 = $manifestHash
        canonical_path = 'Docs/AI_ENGINE_INDEX/source-manifest.jsonl'
        summary_path = 'Docs/AI_ENGINE_INDEX/manifest-summary.json'
    }
    human_report = $humanReport
    routes = [ordered]@{
        human_entry = 'Docs/AI_ENGINE_INDEX/README.md'
        architecture = 'Docs/AI_ENGINE_INDEX/ARCHITECTURE.md'
        source_map = 'Docs/AI_ENGINE_INDEX/SOURCE_MAP.md'
        findings_explained = 'Docs/AI_ENGINE_INDEX/FINDINGS.md'
        findings_machine = 'Docs/AI_ENGINE_INDEX/findings.jsonl'
        source_manifest = 'Docs/AI_ENGINE_INDEX/source-manifest.jsonl'
        human_report_docx = 'Docs/AI_ENGINE_INDEX/Voxagine_Engine_Review.docx'
        human_report_builder = 'Docs/AI_ENGINE_INDEX/build-human-report.py'
        refresh_script = 'Docs/AI_ENGINE_INDEX/refresh-index.ps1'
    }
    lookup_hints = @(
        'Filter findings.jsonl by stable id, subsystem, priority, severity, status, evidence path, or symbol.',
        'Filter source-manifest.jsonl by path, subsystem, kind, symbol, or include.',
        'Treat paths and symbols as durable evidence keys; reviewed line numbers can drift.',
        'Open current source before applying a fix because this review is revision-bound.'
    )
}

[System.IO.File]::WriteAllText($indexPath, ($index | ConvertTo-Json -Depth 12), $utf8NoBom)

Write-Host "Indexed $($manifestRecords.Count) files and validated $($findingRecords.Count) findings."
Write-Host "Review baseline matches HEAD: $($index.repository.baseline_matches_head)"
Write-Host "Manifest: $manifestPath"
Write-Host "Index:    $indexPath"
