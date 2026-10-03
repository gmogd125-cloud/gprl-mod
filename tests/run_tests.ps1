# Builds and runs the geode/core host tests with cl.exe (no Geometry Dash, no Geode SDK needed),
# exactly like D:\GeodeMods\frame-perfect-counter\tests\run_tests.ps1. Exits non-zero on any failure.
#   -Write   determinism_tests (re)generates the `golden` blocks of
#            tests\fixtures\solver\determinism-local-window.json in place (only when every check
#            passes). The file is rewritten in JSON.stringify(v, null, 2) layout; the root
#            `npm run format:check` covers tests\, so run `npx prettier --write` on it afterwards
#            (number arrays collapse again, objects stay expanded; values are unchanged).
#            Only for an intended solver change: the goldens pin the exact trial sequence.
#   -Only a,b  run only these suites (comma separated names without .cpp)
#   -OutDir d  build into another folder than build\tests (two runs in the same folder at the same
#              time overwrite each other's object files)
param([switch]$Write, [string]$Only = '', [string]$OutDir = '')
$ErrorActionPreference = 'Continue'
. 'D:\GeodeMods\_msvc\msvc-env.ps1'
$geode = Split-Path $PSScriptRoot -Parent      # D:\GPRL\geode
$repo = Split-Path $geode -Parent              # D:\GPRL
$out = if ($OutDir) { $OutDir } else { Join-Path $geode 'build\tests' }
New-Item -ItemType Directory -Force $out | Out-Null
Set-Location $out

$core = @(
    "$geode\core\json.cpp",
    "$geode\core\telemetry.cpp",
    "$geode\core\crypto.cpp",
    "$geode\core\calibration.cpp",
    "$geode\core\fingerprint.cpp",
    "$geode\core\classify.cpp",
    "$geode\core\config.cpp",
    "$geode\core\identity.cpp",
    "$geode\core\ranks.cpp",
    "$geode\core\geometry_hash.cpp",
    "$geode\core\fingerprint_build.cpp",
    "$geode\core\solver\boundary_search.cpp",
    "$geode\core\solver\local_window.cpp",
    "$geode\core\solver\pass_planner.cpp",
    "$geode\core\solver\timeline.cpp",
    "$geode\core\solver\window_event.cpp",
    "$geode\core\solver\sequence.cpp",
    "$geode\core\solver\sequence_adjusted.cpp",
    "$geode\core\solver\compensation.cpp",
    "$geode\core\solver\timing_result_event.cpp",
    "$geode\core\display.cpp",
    "$geode\core\clip.cpp",
    "$geode\core\clip_flow.cpp",
    "$geode\core\live_recalc.cpp",
    "$geode\core\entitlements.cpp",
    "$geode\core\identity\gameplay_fingerprint.cpp",
    "$geode\core\identity\sections.cpp",
    "$geode\core\identity\presentation.cpp",
    "$geode\core\identity\level_identity.cpp",
    "$geode\core\sim\physics.cpp",
    "$geode\core\sim\collision.cpp",
    "$geode\core\sim\engine.cpp",
    "$geode\core\sim\gameplay_hash.cpp",
    "$geode\core\sim\search.cpp",
    "$geode\core\sim\windows.cpp",
    "$geode\core\sim\verify.cpp",
    "$geode\core\sim\analysis.cpp",
    "$geode\core\sim\job.cpp",
    "$geode\core\updater.cpp"
)

$suites = @(
    'boundary_search_tests',
    'local_window_tests',
    'telemetry_roundtrip_tests',
    'crypto_tests',
    'fingerprint_tests',
    'calibration_tests',
    'classify_tests',
    'config_tests',
    'identity_tests',
    'menu_tests',
    'determinism_tests',
    'pass_planner_tests',
    'timeline_tests',
    'window_event_tests',
    'geometry_hash_tests',
    'fingerprint_build_tests',
    'activation_tests',
    'isolation_tests',
    'portal_model_tests',
    'live_state_tests',
    'settle_tests',
    'dual_rules_tests',
    'tuning_tests',
    'display_tests',
    'clip_tests',
    'clip_flow_tests',
    'sequence_tests',
    'sequence_adjusted_tests',
    'timing_units_tests',
    'timing_status_tests',
    'cluster_tests',
    'result_ledger_tests',
    'trace_tests',
    'timing_result_event_tests',
    'attribution_tests',
    'trace_view_tests',
    'miss_attribution_tests',
    'live_recalc_tests',
    'entitlements_tests',
    'sim_modes_tests',
    'analyzer_recorder_tests',
    'analyzer_extract_tests',
    'analyzer_identity_glue_tests',
    'analyzer_worker_tests',
    'level_identity_tests',
    'sim_physics_tests',
    'sim_engine_tests',
    'sim_no_gd_symbols_tests',
    'sim_hash_tests',
    'sim_search_tests',
    'sim_windows_tests',
    'sim_verify_tests',
    'sim_job_tests',
    'compensation_tests',
    'ship_pack_tests',
    'updater_tests'
)
if ($Only) {
    $wanted = $Only.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $suites = $suites | Where-Object { $wanted -contains $_ }
}

$code = 0
foreach ($suite in $suites) {
    $src = "$geode\tests\$suite.cpp"
    Write-Output "== $suite"
    cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 $src @core /Fe:"$suite.exe" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Output "$suite compile failed ($LASTEXITCODE)"
        # re-run without Out-Null so the errors are visible
        cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 $src @core /Fe:"$suite.exe"
        $code = 1
        continue
    }
    $extra = @()
    if ($Write -and $suite -eq 'determinism_tests') {
        $extra = @('--write', "$repo\tests\fixtures\solver\determinism-local-window.json")
    }
    if ($Write -and $suite -eq 'level_identity_tests') {
        # (re)generates tests\fixtures\identity\<case>.json + cases.json (only when every check passes)
        $extra = @('--write')
    }
    & ".\$suite.exe" $repo @extra
    if ($LASTEXITCODE -ne 0) { $code = 1 }
}

if ($code -eq 0) { Write-Output "ALL HOST TESTS PASSED" } else { Write-Output "HOST TESTS FAILED" }
exit $code
