# Changelog

All notable changes to this plugin are documented here.

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
