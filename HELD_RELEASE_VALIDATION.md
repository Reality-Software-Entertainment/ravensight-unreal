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
