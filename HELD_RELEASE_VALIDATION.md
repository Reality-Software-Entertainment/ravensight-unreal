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
  and `/tmp/unreal-reports-build.log`. This is an editor test, not a packaged device or live storage upload test.
- No remote writes, publication or deployment. Live storage upload, device and game capture checks remain held.

## 0.5.0: delivery, ads and attribution, exposure and device fields (November 6, 2026)

- Release 0.5.0, with the 8 KB check counting a typed error's trail one byte a
  character: Unreal Engine 5.8.3 Mac Development editor target built with
  `-WarningsAsErrors`; full `Ravensight` automation run **193 passed** (180
  clean, 13 with expected warnings), 0 failed, 0 not run; host check **150
  cases passed**. The iOS exclusion is still compiled only by an iOS build.
- After the backup and temporary file hardening (exclusion before the move,
  checked with one retry, temporary file removed on failure and on every delete,
  folder and older files excluded at load): host check **150 cases passed**; full
  `Ravensight` automation run **193 passed** (180 clean, 13 with expected warnings),
  0 failed. The iOS exclusion itself is still compiled only by an iOS build.
- After the iOS do-not-backup attribute on the queue file: host check **145
  cases passed**; full `Ravensight` automation run **188 passed** (174 clean, 14
  with expected warnings), 0 failed. The new `Private/IOS/RavensightIOSBackupExclusion.mm`
  is compiled only for iOS, and this engine install has no iOS target, so it is
  not compiled here: an iOS build must confirm it.
- After the shared error queue rules (order, full queue, no trail on disk):
  host check **145 cases passed**; full `Ravensight` automation run **188 passed**
  (176 clean, 12 with expected warnings), 0 failed.
- After merging the 0.4.0 error capture from main: engine-free host check
  **143 cases passed** and the full `Ravensight` automation run **184 passed**
  (171 clean, 13 with expected warnings), 0 failed, on the same UE 5.8.3 build.
- Before the merge: engine-free host check **124 cases passed**, including every shared delivery
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

## 0.6.0: identify players who sign in (November 19, 2026)

- Branch `feat/identify`, built to the platform's identify contract version 2.
- Host check: **167 cases passed** (150 before), plus the 4 report hash cases.
- Unreal Engine 5.8 Mac Development editor target of a scratch host project,
  built with `-WarningsAsErrors` (UnrealHeaderTool included) from commit
  26e6c2a: passed. Full `Ravensight` automation run: **218 passed** (201 clean,
  17 with expected warnings), 0 failed, 0 not run, among them the 17
  engine-free identify cases and 8 subsystem identify cases.
- Fake transport only: no live Ravensight service, packaged game or device
  was used. Nothing was published, tagged or deployed.

### Campaign links (same 0.6.0)

- Branch `feat/campaign-link`, on `feat/identify` at 1abf2e8, built to the
  campaign link contract (the JS SDK is the reference). Native only: Unreal
  has no web build, so the game passes the id with `SetCampaignLink`.
- Host check: **173 cases passed** (170 before), plus the 4 report hash
  cases. The 3 new cases run 36 vectors: the id check and `rs_link` read from
  query strings and whole addresses.
- Unreal Engine 5.8 Mac Development editor target of a scratch host project,
  built with `-WarningsAsErrors` (UnrealHeaderTool included) from commit
  21faa33: passed. Full `Ravensight` automation run: **234 passed** (215
  clean, 19 with expected warnings), 0 failed, 0 not run, among them the 3
  engine-free campaign link cases and 10 subsystem campaign link cases (once
  per session, lower case, kept after a failed or token-less answer, a link
  set while the session is on the wire, opt in, opt out, no key, a Playtest
  run, nothing saved).
- Fake transport only: no live Ravensight service, packaged game or device
  was used. Nothing was published, tagged or deployed.
