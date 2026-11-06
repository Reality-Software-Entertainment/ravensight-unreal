# Held combined release, October 17, 2026

Public-build support and typed events have been combined locally. This record
is verification evidence, not a publication or deployment record.

Validated: 128 Unreal Engine automation tests (119 without warnings, 9 with warnings), 103 host checks.

The merge preserves public-key consent and inert-without-key behavior,
stable event IDs, typed event helpers and opt-in performance sampling.
JavaScript now defers performance listeners until consent; Android health
summaries carry the same deduplication IDs as other events.

Physical-device and live-service validation is not implied by these checks.
Publishing, tags, marketplace submission and deployments remain held pending
owner approval. Older README test counts describe their earlier branch runs;
this file records the combined candidate.

## 0.3.1 remote configuration and experiments

Validated locally with Unreal Engine 5.8.3 and Xcode 27 on October 17, 2026:

- UHT and the Mac Development editor target compiled successfully with warnings treated as errors.
- 134 engine automation cases passed: 125 without warnings and 9 expected-warning cases.
- 106 engine-free host checks passed, including config identifier, ETag and parser nesting guards.
- New subsystem specs cover automatic fetch, typed fallbacks, exact ETag/304 reuse, consent gating, stale identity discard, exposure timing and malformed response fallback.

The existing scratch HostGame harness was refreshed from this candidate. Its SDK HTTP requests use the fake transport; no live Ravensight service was exercised. The compiler's local staging step is not a remote deployment.
Packaged game builds, other engine versions and physical devices were not tested for this patch.
No pushes, tags, marketplace submissions, publishing or remote deployments were performed.

## Player Reports follow-up

- Engine-free host checker: **106 cases passed**, plus **4 portable SHA-256 cases**
  covering empty input, standard vector, one-megabyte vector and embedded NUL bytes.
- Added real Unreal subsystem automation cases for preparation, binary upload, absent
  credentials, processing/finalization, stable retry, identity reset and opt-out.
- Host checker does not compile the subsystem or run UnrealHeaderTool.
- Unreal 5.8.3 Mac Development editor build passed with `-WarningsAsErrors`, including
  UnrealHeaderTool generated Blueprint reflection. Full native automation: **136 passed**
  (127 clean + 9 expected-warning cases), zero failures/not-run, process exit 0. Both new
  prepared-report cases passed. Evidence: sibling `validation/unreal-reports-automation/index.json`
  and `/tmp/unreal-reports-build.log`. This is an editor test, not a packaged device or live S3 test.
- No remote writes, publication or deployment. Live S3/device/game capture checks remain held.

## Unreleased: delivery, ads and attribution, exposure and device fields (November 6, 2026)

- Engine-free host check: **124 cases passed**, including every shared delivery
  vector (header and beacon signatures, the 8 KB measure), the error-behind-gameplay
  queue rules and the new response table, plus 4 portable SHA-256 cases.
- Unreal Engine 5.8.3 Mac Development editor target built with `-WarningsAsErrors`
  (UnrealHeaderTool included) in a scratch host project.
- Full `Ravensight` automation run: **159 passed** (149 clean, 10 with expected
  warnings), 0 failed, 0 not run. New subsystem cases cover the queue file read back
  under its original origin, signing over the plain body with gzip, a batch 403 kept
  for the next launch, the session's device fields, ads and attribution, and exposure.
- Requests went through the fake transport; no live Ravensight service, packaged
  build or device was exercised. No push to GitHub, tag or publication.
