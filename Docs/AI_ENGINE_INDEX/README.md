# Voxagine engine review and AI index

This directory is the durable, AI-oriented map of the Voxagine engine. It combines a full static architecture/correctness review with deterministic source metadata so a future agent can answer “where does this happen?”, “what owns this?”, and “what is already known to be risky?” without rescanning the entire repository.

## Reviewed baseline

- Repository: Voxagine
- Revision: `743c4c67331b10bf7b7d732f78dedc9c9e2215d4`
- Commit: `Phase 4d: shrink the CPU voxel from 16 B to 4 B, owner beside it (#15)`
- Commit timestamp: `2026-08-08T23:44:00+02:00`
- Review date: 2026-08-09
- Primary engine surface: `Voxagine/Source/Core`
- Supporting surfaces: `Voxagine/Source/Editor`, `Game/Source`, `UnitTesting/UnitTests/Source`, root/CMake build metadata, and CI configuration
- Deliberately excluded from line-by-line review: vendored code under `Voxagine/Source/External`, binary assets, generated build products, `.git`, `.idea`, and compressed distributables

The worktree remained active during the review. User/IDE-owned changes observed outside these review artifacts include `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `Voxagine/Source/Core/Utils/DateTime.h`, and Rider `.idea` run configurations; untracked runtime settings files also appeared under `Voxagine/Source`. They were preserved and were not authored or reverted by this review. The revision above remains the Git baseline, while the generated source/document hashes identify the exact final snapshot.

## Artifact map

| Artifact | Canonical use |
| --- | --- |
| `index.json` | First machine-readable entry point: schema, baseline, aggregate counts, routes, and verification state |
| `review-metadata.json` | Curated baseline, scope, methodology, and build/test verification input |
| `findings.jsonl` | Canonical issue database; one independent JSON object per line |
| `source-manifest.jsonl` | Generated first-party source inventory; one file per line |
| `manifest-summary.json` | Generated source counts and content hash |
| `ARCHITECTURE.md` | Human-readable lifecycle, ownership, threading, and subsystem architecture |
| `SOURCE_MAP.md` | Fast task/symbol-to-source routing table |
| `FINDINGS.md` | Human-readable explanation of every stable finding |
| `Voxagine_Engine_Review.docx` | Polished human report compiled from all canonical findings |
| `build-human-report.py` | Deterministic DOCX builder; reads the central index, metadata, and findings JSONL |
| `refresh-index.ps1` | Deterministically regenerates the source manifest, summary, and central index |

## Executive assessment

Voxagine is a substantial C++17 voxel engine/game stack with a coherent high-level split: `Application` composes platform, world, jobs, resources, settings, serialization, and optional editor services; each `World` owns staged ECS entities and systems; Vulkan rendering uses a voxel bake/grid path; RTTR reflection drives serialization and editor property handling.

The pulled voxel pass is materially positive. It reduces each CPU voxel to four bytes, moves ownership to a compact sidecar, adds brick occupancy and far-field validation, improves voxel-range checks, and adds codec auditing. Those changes lower memory pressure and make several rendering invariants more explicit.

The dominant remaining risk is not the packed voxel representation. It is lifetime coordination across asynchronous jobs and raw-owned engine objects. Job-system teardown can race its workers, world/chunk/pathfinding/physics jobs capture raw objects, and several main-thread systems are destroyed before their outstanding work is conclusively canceled and joined. The next risk cluster is malformed or partial input: JSON/VOX/chunk decoding contains unchecked shapes, counts, or buffers that can become invalid memory access. Editor undo history and the legacy allocators contain deterministic use-after-free/corruption paths. Input’s specific-player branches contain deterministic out-of-range indexing.

CI currently proves only the Vulkan bring-up/RTTR dependency surface on Ubuntu. The full game/editor and the legacy GoogleTest suite are not CI targets, so the highest-risk ownership and parsing boundaries have little automated protection.

## Finding semantics

Every record has a stable ID and these dimensions:

- `priority`: remediation order. `P0` means memory corruption, use-after-free, unbounded work, process termination, or a central supported path can fail. `P1` is a significant correctness/resilience problem. `P2` is localized, dormant, performance, or maintainability debt. `P3` is informational.
- `severity`: consequence if triggered: `critical`, `high`, `medium`, `low`, or `info`.
- `status`: evidence type, not workflow state.
  - `confirmed` — deterministic from the current code.
  - `concurrency-risk` — an unsynchronized interleaving exists; stress/sanitizer evidence is still desirable.
  - `needs-runtime-validation` — plausible and source-supported, but dependent on runtime/API behavior.
  - `coverage-gap` — missing verification rather than a direct defect.
  - `limitation` — intentional or architectural constraint to preserve in future changes.
- `confidence`: `high`, `medium`, or `low` confidence in the stated interpretation.

Line numbers in evidence are navigation hints for the reviewed baseline. Paths and symbols are the durable keys because later edits will move lines.

## Efficient AI lookup protocol

1. Read `index.json` first. It is small and tells you whether the review baseline matches the source snapshot.
2. Search `findings.jsonl` by subsystem, path, symbol, or stable ID. Do not ingest every source file to answer a known-risk question.
3. Search `source-manifest.jsonl` for the symbol or include when the implementation location is unknown.
4. Read the relevant architecture/source-map section, then open only the routed files.
5. Treat `Voxagine/Source/External` as third-party unless the question is explicitly about integration or vendored modifications.
6. For a fix, re-open current source before patching. Findings describe the reviewed baseline and may become stale after later commits.

Useful filters:

```powershell
rg '"subsystem":"pathfinding"' Docs/AI_ENGINE_INDEX/findings.jsonl
rg '"status":"concurrency-risk"' Docs/AI_ENGINE_INDEX/findings.jsonl
rg '"evidence":.*JsonSerializer' Docs/AI_ENGINE_INDEX/findings.jsonl
rg '"subsystem":"rendering"' Docs/AI_ENGINE_INDEX/source-manifest.jsonl
```

## Refresh and maintenance

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Docs/AI_ENGINE_INDEX/refresh-index.ps1
```

The script:

1. scans only first-party source/build/test roots;
2. extracts likely declared/qualified symbols and includes;
3. records lines, bytes, and SHA-256 for each file;
4. validates every JSONL finding and rejects duplicate IDs;
5. regenerates `source-manifest.jsonl`, `manifest-summary.json`, and `index.json` as UTF-8 without BOM;
6. reports whether the curated review baseline still matches `HEAD`.

When code fixes a finding, retain its ID and change the record to a resolution status only after adding evidence and validation. When the reviewed baseline changes substantially, update `review-metadata.json`, rerun the review for affected surfaces, then regenerate the index.

The Word report can be rebuilt with the workspace dependency runtime:

```powershell
& 'C:\Users\Arthur\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' Docs/AI_ENGINE_INDEX/build-human-report.py
```

For this review, the DOCX accessibility, section, image, heading-count, placeholder, and exact table-geometry audits passed. The normal page-render gate could not run because neither LibreOffice nor Microsoft Word is installed on the machine; that limitation is recorded in `review-metadata.json` and the report verification table.

## Review limits

This was a deep static review plus compile verification, not a claim of exhaustive runtime proof. It did not launch a graphical Vulkan/FMOD session, fuzz untrusted assets, run thread sanitizers, or dynamically exercise every game/editor path. Concurrency records distinguish deterministic lifetime gaps from issues that still need stress/sanitizer confirmation. The test/verification gaps themselves are indexed so future work can close them systematically.
