# Changelog

All notable changes to this plugin are documented here.

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
