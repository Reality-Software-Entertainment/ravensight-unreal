# Changelog

All notable changes to this plugin are documented here.

## [0.4.0] - 2026-11-06

- Add automatic error capture, on by default with a new **Error Capture**
  project setting to opt out. An output device on `GLog` captures `Error`
  lines (`rs_source` `log`, severity `warning`) and `Fatal` lines
  (`critical`); `FCoreDelegates::OnEnsureFailed` captures ensures with
  their stack; `FBlueprintCoreDelegates::OnScriptException` captures
  Blueprint script exceptions (`uncaught`, `error`). Hooks copy into a lock
  protected list from any thread; the subsystem drains it on the game
  thread.
- Nothing is installed without a key, before consent, or while the game's
  `errorCapture.enabled` is false. Up to 5 boot errors are held until the
  settings or session answer and discarded unsent when it says off.
- Device caps per session: 30 captures, 3 per dedupe key, 10 trails, 6 a
  minute. Message, stack and trail are scrubbed on the device with the
  platform's shared fixture, which the specs run case for case.
- Optional trail of the last 20 log lines, attached only when the game's
  trail switch is on; with it off, Warning and lower lines are dropped
  before any copy.
- Cheap when spent: the caps and the bucket are checked before any scrub or
  dedupe work, the hook list keeps separate quotas for errors and trail
  lines, and the hooks stop copying errors once the session's captures are
  spent. Builds without logging (`NO_LOGGING`, the Shipping default) skip the
  log device and keep the ensure and Blueprint hooks.
- Gameplay events are queued ahead of error captures, and error captures are
  dropped first when the queue is full. A `413` on a batch is handled like a
  `400`.

## [0.3.1] - Unreleased (held)

- Add Blueprint prepared Player Reports, bounded encoded images, stable session-bound retries, explicit status/cancel and local handle cleanup.
- Upload binary multipart through a fresh credential-free HTTPS request; reject stale consent/identity callbacks and keep synthetic screenshots in playtest artifacts.

- Add native and Blueprint remote config getters, refresh and readiness delegate.
- Fetch automatically at session start, cache by the complete player/build context, and reuse exact ETags.
- Add bounded config validation and automatic experiment exposure labels at event enqueue time.
- Clear active config and reject stale responses after opt-out or identity changes.
- Adapt JSON key iteration to UE 5.8 shared-string keys and add config protocol/subsystem specs.

## [0.3.0] - 2026-10-16

For games whose builds or source anyone can download. A project that sets
only its key and none of the new settings behaves as on 0.2.1, except for
the changes listed first. Still a beta: not yet run in a packaged game or on
a device.

### Changes for 0.2.1 projects

* A `401` or `403` to the settings check or to `POST /session` (other than
  `tracking_disabled` and a refused Playtest token) now ends the run: the
  queue is cleared, the timers stop, nothing more is sent, and
  `OnTrackingDisabled` fires with the new source `key`. In 0.2.1 the events
  stayed queued and `Flush()` made one more attempt. The status reason is
  still `invalid_key`, logged once. A batch `401` still opens a new session
  first; a later batch `403` still only stops the automatic retries.
* Retry waits carry plus or minus 20 percent jitter, never under half a
  second and never over 5 minutes. `Retry-After` is still obeyed exactly.
  `FRavensightQueue::SetRandomSource` replaces the source; one that returns
  0.5 gives the 0.2.1 ladder exactly.
* Every event's data gains an `rs_event_id` key, and an event with no data is
  sent with that one key instead of `{}`.
* An event whose data is over 8,192 bytes as JSON is dropped before it is
  queued, with one warning for the run and `OnFlushFailed("event_too_large")`.
  In 0.2.1 it was sent and dropped on the server's `400`.
* `Init` with an empty key, or auto initialize with an empty Ingest Key,
  sets the status reason to `no_key` and drops anything tracked so far.
* `LoadTrackingEnabled()` reads the choice for the project's Consent setting;
  with the default it returns what it did before.

### Added

* **Inert without a key.** An empty Ingest Key writes nothing to
  `GameUserSettings.ini`, makes no request and starts no timer, and every
  call is a no-op until an `Init` with a key. One warning, silenced by the new
  **Optional** setting.
* **`IsEnabled()`**, true only while the plugin is collecting, and
  `GetDistribution()`.
* **Consent** (`ERavensightConsent`, Opt Out by default, or Opt In), a project
  setting. With Opt In nothing is sent, no device id is made and tracked
  events are dropped until `SetTrackingEnabled(true)`. The yes is saved as
  `[Ravensight] OptIn` in `GameUserSettings.ini`; a no removes it and keeps
  the device id. Events tracked right after the yes are queued while the
  settings check runs. `ResetDeviceId` without a current yes forgets the
  saved id and makes none.
* **Distribution**, a project setting: a label of at most 40 characters, sent
  on `POST /session` as `distribution`. A label that is too long or has a
  control character is not sent and is logged once.
* **`rs_event_id`** on every event, including `game_exited` sent under an old
  session: the first 16 characters of the device id, the start time in hex,
  an 8 hex character launch nonce from `FGuid::NewGuid`, and a counter.
  Events tracked before `Init` get the device part when `Init` runs. A batch
  sent again carries the same ids. `RavensightProtocol::EventId`,
  `EventIdPrefix` and `ValidDistribution` are pure and covered by the host
  check.
* README: a "Public builds" section and a compatibility table.
* Specs: the jitter (`Ravensight.Queue`, 4 cases), the distribution label and
  `rs_event_id` (`Ravensight.Protocol`, 4 cases), and in `Ravensight.Subsystem`
  (13 cases) an inert start, Init with an empty key then a real one, Opt In
  before and after the yes, revoking, the distribution on the session body,
  a refused key on the settings check and on the session, a batch `401`
  still refreshing, the id shape, prefix and uniqueness, a resend carrying
  the same ids, and the 8 KB cap.

### Verified

* Unreal Engine 5.8 on macOS (Xcode 27.0), a scratch host project: the
  editor target builds with warnings and shadowed variables as errors, with
  no warnings; `RunUAT.sh BuildPlugin -TargetPlatforms=Mac` succeeds for the
  editor and the game in Development and Shipping; the automation specs run
  headless in the editor, **99 tests, 99 passed**; the host check, 74 cases,
  0 failed.
* Not run: a packaged game, iOS, Android, Windows, Linux, and any engine
  version other than 5.8.

### Typed events and performance sampling (included in this release)

Not yet compiled in Unreal. The engine free logic passes the host check
(`bash Tools/host-check/run.sh`); the subsystem changes, the new setting and
the Blueprint signatures need an editor build.

### Added

* Typed event helpers on `URavensightSubsystem`, Blueprint callable under
  Ravensight > Typed Events: `TrackPurchase` (and `TrackPurchaseText` for a
  digit string amount), `TrackResource`, `TrackProgression` and `TrackError`.
  Each sends the reserved `rs_` properties as a JSON object through
  `TrackEventJson`, so numbers stay numbers. Extra properties merge first and
  the reserved keys win; an event name override is optional.
* Opt in performance sampling, the `Performance` project setting (off by
  default). An `FTSTicker` samples the frame rate once a second into a
  reservoir of at most 600 samples, and one `health` event (median and 10th
  percentile fps, memory in MB, boot time in ms) is sent per session, on
  background entry on iOS and Android and on quit, never from the ticker.
* `RavensightTypedEvents`, the engine free property builders and health
  sampler, with the `Ravensight.TypedEvents` automation spec, also run by the
  host check.
* The session create request now carries `"sdk": "unreal/0.3.0"`.

### Changed

* Version 0.3.0 in `Ravensight.uplugin` and `RavensightVersion::Sdk`.

## [0.2.1] - 2026-10-10

First release compiled in Unreal. Still a beta: not yet run in a packaged
game or on a device (see the README's verification status).

### Verified

* Unreal Engine 5.8.3 on macOS (Xcode 27.0): the editor target builds with
  warnings and shadowed variables as errors, with no warnings. The 0.2.0
  source needed no compile fix.
* The Mac game target compiles and links in Development and Shipping, and
  `RunUAT.sh BuildPlugin -TargetPlatforms=Mac` succeeds for the editor and
  the game, arm64 and x64.
* The automation specs run headless in the editor: 78 tests, 78 passed.

### Added

* `Ravensight.Subsystem`, an automation spec over the real subsystem in a
  real game instance, with every HTTP request recorded instead of sent: a
  start with Enabled off makes no request, a saved opt-out carries into a new
  subsystem instance, suggestions and feedback while tracking is off send
  nothing, the Playtest header is on every request, and `ResetDeviceId`
  drops the old identity and session. Needs Unreal Engine 5.8 or newer.
* An internal transport seam (`RavensightTransport`), where every request is
  created and sent. By default it is the engine's HTTP module, unchanged.
* iOS: the flush on entering the background runs inside a UIKit background
  task, ended once nothing is in flight, on return to the foreground, on
  quit, or by iOS when time runs out. The Objective-C++ file
  (`Private/IOS/RavensightIOSBackgroundTask.mm`) compiles for iOS only and
  has not yet been built by UnrealBuildTool or run on a device: the iOS
  target platform was not installed on the build machine.

### Known limitations

* Not yet run in a packaged game, on iOS or Android, on Windows or Linux, or
  on any engine version other than 5.8.3.
* The iOS background task is unverified until it is built for iOS and run on
  a device.

## [0.2.0] - 2026-10-10

Brings the plugin to the contract every Ravensight SDK now shares. Still a
beta: not yet compiled in Unreal by its maintainers (see the README's
verification status).

### Breaking

* `game_paused` and `game_resumed` are no longer sent by default. Turn on the
  new **Track Pause Events** setting to keep them; they now fire on entering
  the background only, never on desktop focus loss.
* The player's opt-out is saved. `SetTrackingEnabled` writes `[Ravensight]
  TrackingEnabled` to `GameUserSettings.ini` and the plugin reads it on every
  launch before anything is sent. A game that re applied the choice itself can
  keep doing so, but no longer needs to.
* `TrackEvent` only queues. The per-event send is gone: events go out on the
  timer (5 seconds), at 50 queued, on `Flush`, on background and on quit.
* Feedback refused while tracking is off now reports `disabled` (was
  `tracking_disabled`), and a category outside `bug`, `suggestion`,
  `complaint`, `praise`, `playtest`, `other` or a rating outside 0 to 5 fails
  locally with `invalid_category` or `invalid_rating`.
* The default **Api Url** is the bare host `https://api.ravensight.io`; the
  plugin appends `/api/v1`. A saved full versioned URL keeps working.
* A refused ingest key stops automatic retries for the run instead of
  retrying forever, and a settings `401` is no longer read as "tracking
  enabled".

### Added

* **Enabled** setting and `SetEnabled(bool)`: off means no request of any
  kind. Not saved.
* `ResetDeviceId()`, `SaveTrackingEnabled(bool)` and `LoadTrackingEnabled()`.
* `GetStatusReason()` and `OnStatusChanged`, with the shared reason values
  `disabled`, `invalid_key`, `tracking_disabled`, `offline`, `rate_limited`,
  `session_failed` and `playtest_job_closed`.
* Ravensight Playtest runs: `RAVENSIGHT_PLAYTEST_TOKEN` and
  `RAVENSIGHT_PLAYTEST_RUN_ID` (or `--ravensight-playtest-token=` and
  `--ravensight-playtest-run-id=` on desktop), `SetPlaytestContext`,
  `IsPlaytest()`, an in-memory `pt-<runId>` device id and the
  `X-Ravensight-Playtest` header on every request.
* `GetSessionToken()`, for handing the session to a game server as
  `joinToken`.
* `GetFeedbackCategories()`.
* iOS and Android follow the 30 minute background rule (**Session Timeout
  Seconds**), with a saved marker so an app swiped away in the background
  reports its exit on the next launch.
* `RavensightProtocol`, the engine-free response table and lifecycle rules,
  with `RavensightProtocol.spec.cpp`.
* `Tools/host-check/run.sh`, which runs both specs on a plain C++17 compiler.
* README sections: automatic events, status reasons, Playtest runs, levels
  and deaths (`rs_kind`), telling players, and an opt-out and reset example.

### Fixed

* An opted out player's machine no longer reads settings, opens a session or
  fetches suggestions.
* `game_started` is queued once per session, at the real start time, however
  often the settings check or an opt in runs. `game_exited` is queued once,
  and the quit is held for at most 1.5 seconds to send it.
* A `403 tracking_disabled` from any request is the kill switch, not a
  retried failure.
* A `400` on a batch halves it, then drops the single event the server will
  not take, instead of blocking the queue for the rest of the run.
* A batch `401` re opens the session at most twice in a row, then backs off.
* A `503` with `Retry-After` waits that long.
* The ingest key is described accurately: it can also read this game's design
  suggestions.
* A batch the server accepts clears a lingering `rate_limited` or `offline`
  reason, and a batch that cannot reach the server reports `offline`.
* An answer to a batch or session request made before the queue was thrown
  away (opt out, Enabled off, `ResetDeviceId`) is ignored.
* A settings `429`, or `503` with `Retry-After`, waits that long.
* The quit flush also waits, within the same 1.5 seconds, for a settings
  check still in flight.

### Known limitations

* Not yet compiled in Unreal; see the README's verification status.
* On iOS the background flush is not wrapped in a background task, so iOS may
  suspend the app before the last batch is sent. The background marker still
  reports the session's exit on the next launch.

## [0.1.0] - 2026-08-28

First public beta.

### Added

* `URavensightSubsystem`, a `UGameInstanceSubsystem` entry point. Blueprint
  callable `Init`, `TrackEvent`, `TrackEventJson`, `Flush`, `SubmitFeedback`,
  `FetchSuggestions` and `SetTrackingEnabled`, plus `BlueprintAssignable`
  delegates for session, flush, feedback and suggestion results.
* `URavensightSettings`, a `UDeveloperSettings` panel under
  Project Settings > Plugins > Ravensight, so the plugin can be configured and
  shipped without writing code.
* `FRavensightQueue`, the complete protocol state machine with no UObject, HTTP
  or engine tick dependency: batching at the 50 event server limit, the offline
  queue with a 500 event drop oldest cap, re authentication on 401,
  `Retry-After` on 429, and exponential backoff from 10 seconds to 5 minutes.
* Server side kill switch honored at boot via `GET /settings`.
* Device id generated as a GUID and persisted to the local game user settings
  ini. No advertising identifier and no hardware identifier is used.
* Automatic flush on a 5 second timer, on `ApplicationWillDeactivate`,
  `ApplicationWillEnterBackground`, `OnWorldBeginTearDown` and `Deinitialize`.
* `RavensightQueue.spec.cpp`, an automation spec covering queue ordering, the
  drop oldest cap, batch sizing, the 401 re send path, `Retry-After` against
  exponential backoff, the full backoff ladder to its ceiling, and both
  enablement switches.

### Known limitations

* Never compiled by UnrealBuildTool or run in the editor. See the beta notice
  in the README and the version sensitive lines listed in `Tools/README.md`.
* The flush on shutdown is best effort. The engine stops pumping the HTTP
  manager shortly after `Deinitialize`, so a process that exits immediately can
  lose the last batch.
* Feedback is not queued. Unlike an event it is tied to a moment in the
  player's session, so a call made before a session exists fails immediately
  with `no_session` rather than being held.
