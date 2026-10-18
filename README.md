# Ravensight for Unreal Engine

Official Unreal Engine 5 plugin for [Ravensight](https://ravensight.io) player
analytics.

Sessions, batched events, an offline queue, rate limit handling and a server
side kill switch, in one game instance subsystem with no third party
dependencies. Everything is exposed to Blueprint, so a designer can ship it
without writing C++.

> **Beta.** Version 0.3.1 passes 106 portable checks and four report hash checks.
> Unreal Engine 5.8.3 builds and 136 automation tests pass on macOS. Packaged games, physical mobile devices and live screenshot storage remain unverified.

* API base: `https://api.ravensight.io` (the plugin appends `/api/v1`)
* Docs: https://ravensight.io/docs/
* Privacy: a random per-install device id, no advertising identifier, a saved player opt-out (`SetTrackingEnabled(false)`), no tracking permission prompt needed. What your store label can say: https://ravensight.io/docs/#privacy-label
* Unreal Engine 5.1 or newer by design; so far built and tested on 5.8.3 only
* Public builds (demos, jam entries, open source games): no key means no
  analytics, an opt-in consent mode and a distribution label. See
  [Public builds](#public-builds).

## Remote configuration and experiments (0.3.1)

Every successful session start fetches public game values through the existing ingest key. `Fetch Config` also refreshes on demand. The request uses the current device, platform and game version; the key selects the game and environment.

In Blueprints, bind **On Config Ready**, then use **Get Config Bool**, **Get Config Number**, **Get Config String** or **Get Config Json**, with a safe fallback for every key. The JSON getter returns object or array JSON only. These getters never coerce types.

```cpp
const double Speed = Ravensight->GetConfigNumber(TEXT("enemy_speed"), 2.0);
const bool ShowHint = Ravensight->GetConfigBool(TEXT("show_hint"), true);
const FString Welcome = Ravensight->GetConfigString(TEXT("welcome_text"), TEXT("Welcome"));
const FString Layout = Ravensight->GetConfigJson(TEXT("layout"), TEXT("{}"));
Ravensight->FetchConfig();
```

`OnConfigReady` runs on the game thread. `bFresh` is true for an accepted response or HTTP 304; a failed refresh reports false and retains cached values. Values are cached under `Saved/Ravensight`, scoped by a hash of endpoint, key, device, platform and build. Playtests use memory only. Config files are read only after consent. Opt-out, disable and identity reset clear active values and discard late responses. A changed context starts from the supplied fallbacks. The next request reuses the exact ETag; HTTP 304 is free, while a fresh config fetch consumes one ordinary event.

Active experiment assignments add `rs_exp_<experiment_key>` to new events as they are queued. Existing queued events are never relabeled, retries retain their original assignments, and caller-supplied exposure labels are removed. Documents are bounded to 50 keys, 8 KB per value, JSON depth 20 and 10 experiment assignments. Invalid responses preserve the last valid snapshot. Use config for public game data, never credentials or secrets.

## Compatibility

| ravensight-unreal | Engine by design | Engine built and spec tested | Server features it uses |
| --- | --- | --- | --- |
| 0.2.1 | 5.1 or newer | 5.8.3 on macOS | Sessions, batches, settings, feedback, Playtest header. |
| 0.3.0 | 5.1 or newer | 5.8.3 on macOS | As 0.2.1, plus `distribution` on `POST /session`, `rs_event_id` in each event's data (the server drops a repeat), and public `gt_pub_` keys. A server that ignores the new field still accepts every request. |

## Verification status

**Beta. Compiled and spec tested in Unreal Engine 5.8.3 on macOS; not yet run
in a packaged game or on a device.**

For 0.3.0, on 2026-10-16, with Unreal Engine 5.8 (installed build) and Xcode
27.0 on an Apple silicon Mac, in a scratch C++ host project with the plugin
copied under `Plugins/`:

* **Editor build.** `Build.sh HostGameEditor Mac Development` with warnings
  and shadowed variables as errors: no warnings, no errors.
* **`RunUAT.sh BuildPlugin -TargetPlatforms=Mac`**: succeeded, the editor
  and the game in Development and Shipping (so the specs compile out of
  Shipping as before).
* **Automation specs in the editor**, headless (`UnrealEditor-Cmd` with
  `-nullrhi`): 99 tests, 99 passed, 0 failed, 0 not run. That is
  `Ravensight.Queue` (28), `Ravensight.Protocol` (46) and
  `Ravensight.Subsystem` (25). Several passing tests carry a warning: the
  SDK's own warning lines for a refused key, an inert start, an oversized
  event and a bad distribution label, which those cases cause on purpose,
  and the editor's iOS device probe.
* **Host check.** `bash Tools/host-check/run.sh`: 74 cases, 0 failed.
* Not run for 0.3.0: the host project's own Mac game target, and everything
  listed as not run below.

What follows is the 0.2.1 record.

What was run, on 2026-10-10, with Unreal Engine 5.8.3 (installed build) and
Xcode 27.0 on an Apple silicon Mac:

* **Editor build.** UnrealHeaderTool and UnrealBuildTool built the plugin into
  a small C++ host project's `HostGameEditor` target (Mac, Development) with
  warnings treated as errors and shadowed variables as errors. No warnings.
  The 0.2.0 source compiled unchanged; nothing needed a fix.
* **Mac game target.** The same host project's `HostGame` target compiled and
  linked in Development and Shipping. The step after the link, where Xcode
  wraps the binary in a `.app`, failed on this machine in an Xcode scheme
  script ("Touch UBT generated tiles") that belongs to the engine's generated
  project, not to the plugin. So no `.app` was produced or launched.
* **`RunUAT.sh BuildPlugin -TargetPlatforms=Mac`.** Succeeded: the plugin
  built for `UnrealEditor` and `UnrealGame`, arm64 and x64.
* **Automation specs in the editor**, headless (`UnrealEditor-Cmd` with
  `-nullrhi`): 78 tests, 78 passed, 0 failed, 0 not run. That is
  `Ravensight.Queue` (24), `Ravensight.Protocol` (42) and the new
  `Ravensight.Subsystem` (12). A passing test can carry an engine warning
  unrelated to the plugin (the editor's iOS device probe could not start).
* **Host check.** `bash Tools/host-check/run.sh`: the queue and protocol specs
  on a plain C++17 compiler, 66 cases, 0 failed.

`Ravensight.Subsystem` drives a real `URavensightSubsystem` in a real game
instance, with every HTTP request recorded instead of sent. It covers: a
start with the Enabled setting off makes no request of any kind; a saved
opt-out carries into a new subsystem instance, which makes no request;
suggestions and feedback while tracking is off answer at once and send
nothing; the Playtest token goes out in `X-Ravensight-Playtest` on the
settings, session and batch requests; `ResetDeviceId` saves a new id,
empties the queue, ignores a late answer for the old session and opens the
next session under the new id.

What has **not** been run: a packaged game; any iOS or Android build or
device; Windows or Linux; any engine version other than 5.8.3. The
`Ravensight.Subsystem` spec implements the engine's `IHttpResponse` for its
fake answers, and that interface changes between versions, so that one spec
compiles only on 5.8 or newer; the other two have no such limit.

iOS background flush: on entering the background the flush now runs inside a
UIKit background task (`beginBackgroundTaskWithName:expirationHandler:`),
ended once no settings, session or batch request is in flight, on return to
the foreground, on quit, or by iOS's own expiration handler. It lives in
`Source/Ravensight/Private/IOS/RavensightIOSBackgroundTask.mm`, which
UnrealBuildTool compiles only for iOS. The iOS target platform was not
installed on the build machine, so that file has **not** been compiled by
UnrealBuildTool or run on a device. It was only syntax checked with Xcode's
clang against the iPhoneOS 27.0 SDK, with stand-in headers for the two
engine types it uses. It compiles for real once the iOS target is installed
(`-TargetPlatforms=Mac+IOS`). Until it has run on a device, treat it as
unverified; the background marker still lets the next launch report the
session's exit either way.

Read `Tools/README.md` before you build. It lists the exact commands to compile
check the plugin and run the specs, and the specific lines most likely to need
a nudge for your engine version. Please open an issue with anything you hit.

## Install

### Into a project

Drop the plugin into your project's `Plugins` folder:

```
YourProject/
  YourProject.uproject
  Plugins/
    Ravensight/
      Ravensight.uplugin
      Source/
```

```bash
mkdir -p YourProject/Plugins
git clone https://github.com/Reality-Software-Entertainment/ravensight-unreal.git YourProject/Plugins/Ravensight
```

Then right click your `.uproject` and choose **Generate Visual Studio project
files** (or **Generate Xcode project**), and build. The editor picks the plugin
up on next launch and enables it automatically.

### As a submodule

```bash
git submodule add https://github.com/Reality-Software-Entertainment/ravensight-unreal.git Plugins/Ravensight
git submodule update --init --recursive
```

Pin a release with `cd Plugins/Ravensight && git checkout <tag>`, for
example `v0.3.0`.

### Blueprint only projects

The plugin is C++, so the project needs a C++ target to compile it. If yours is
Blueprint only, add any empty C++ class once from the editor
(**Tools > New C++ Class**) and Unreal converts the project for you. You never
have to open the C++ file again.

## Quickstart

### 1. Set your ingest key

Open **Edit > Project Settings**, scroll the left hand list to the **Plugins**
group at the bottom, and select **Ravensight**. The panel has two sections:

* **Connection**: `Api Url` (already filled in with
  `https://api.ravensight.io`), `Ingest Key`, `Optional`, `Distribution`,
  `Game Version`.
* **Behavior**: enabled, auto initialize, consent, flush interval, queue size,
  lifecycle and pause events, session timeout, request timeout, plus a
  **Diagnostics** section with verbose logging.

Paste your publishable ingest key (`gt_live_...`) into **Ingest Key**. Leave
**Game Version** empty to use the project version from
**Project Settings > Project > Description**.

That is the whole setup. The values are written to `Config/DefaultGame.ini`, so
they travel with the project and ship in packaged builds. Nothing else is
required: on the next Play In Editor the subsystem reads the player's saved
choice, reads the kill switch, opens a session and starts flushing on a timer.

### 2. Track an event from Blueprint

In any Blueprint graph:

1. Right click, search for **Get Ravensight**, and place the node. It needs no
   target pin, only the world context Unreal fills in for you.
2. Drag from its return pin and search for **Track Event**.
3. Fill in **Event Name**, for example `level_completed`.
4. The **Data** pin is a map of string to string. Click the plus icon on it once
   per property, then type the key on the left and the value on the right.

A finished node chain reads left to right: `Get Ravensight` into `Track Event`,
with an execution wire coming from whatever fired the moment you care about,
such as a Level Complete custom event.

For numbers or nesting, use **Track Event Json** instead and pass a JSON object
string such as `{"level":3,"score":15000,"time_seconds":87.4}`.

To react to results, select the `Get Ravensight` node's output, drag off it and
choose **Assign On Events Flushed** (or any other delegate in the list). Unreal
creates the bind node and a matching custom event for you.

### 3. Or track from C++

```cpp
#include "RavensightSubsystem.h"

void AMyGameMode::OnLevelCleared(int32 Level, int32 Deaths, float Seconds)
{
    if (URavensightSubsystem* Ravensight = URavensightSubsystem::Get(this))
    {
        Ravensight->TrackEventJson(
            TEXT("level_completed"),
            FString::Printf(
                TEXT("{\"level\":%d,\"deaths\":%d,\"seconds\":%.1f}"),
                Level, Deaths, Seconds));
    }
}
```

Add `"Ravensight"` to your module's `PublicDependencyModuleNames` in
`YourProject.Build.cs` first.

For string only payloads the `TMap` overload is friendlier:

```cpp
TMap<FString, FString> Data;
Data.Add(TEXT("weapon"), TEXT("longbow"));
Data.Add(TEXT("biome"), TEXT("marsh"));
Ravensight->TrackEvent(TEXT("enemy_defeated"), Data);
```

### 4. Gate it behind a consent prompt

You do not need a prompt (see [Telling players](#telling-players)), but if
you want one, turn **Auto Initialize** off in Project Settings, then start the
SDK yourself once the player has answered:

```cpp
Ravensight->Init(TEXT("gt_live_your_key"));
```

`Init` is also **Init** in Blueprint, with optional Api Url and Game Version
pins that fall back to the project settings when left empty.

### 5. Levels and deaths

Ravensight builds the level funnel, per-level deaths and journeys from event
names: names containing `died`, `destroyed` or `killed` count as deaths,
`start`, `begin` or `enter` as level starts, and `complet`, `finish`, `win`
or `clear` as completions. The `level`, `location` or `track` field says
where it happened.

To say it explicitly, set `rs_kind` in the event data to `level_start`,
`level_complete`, `death` or `none`. It is read before the name, and `none`
opts an event out, so `boss_phase_begin` is not a level start. An unknown
value falls back to the name. Keys that start with `rs_` are reserved for
Ravensight.

```cpp
Ravensight->TrackEventJson(TEXT("boss_phase_begin"), TEXT("{\"level\":\"castle\",\"rs_kind\":\"none\"}"));
Ravensight->TrackEventJson(TEXT("run_over"), TEXT("{\"level\":\"castle\",\"rs_kind\":\"death\"}"));
```

In Blueprint, add `rs_kind` as one more key on the **Track Event** Data map.

### 6. Typed events

Four helpers send the reserved `rs_` properties for you, so purchases, resource
flows, progression and errors show up in the typed dashboard views and the
analyst without hand writing keys. Each helper builds a JSON object (numbers
stay numbers) and sends it through `TrackEventJson`, so queueing, batching,
opt out and Playtest behave exactly as for any other event. Every helper takes
an `Extra` map, merged first so the reserved keys win, and an `EventName`
override; leave it empty for the default name.

```cpp
// A 4.99 USD purchase. The amount is whole minor units.
Ravensight->TrackPurchase(499, TEXT("usd"), TEXT("gem_pack"), TEXT("gems_500"),
    TEXT(""), TEXT("order-1042"), {}, TEXT(""));

// Gold gained from a quest, then spent in a shop. Flow is "source" or "sink".
Ravensight->TrackResource(TEXT("source"), TEXT("gold"), 120.0, TEXT("quest"), TEXT("q7"), {}, TEXT(""));
Ravensight->TrackResource(TEXT("sink"), TEXT("gold"), 45.0, TEXT("shop"), TEXT("potion"), {}, TEXT(""));

// Progression: Status is start, complete or fail; P1 to P3 narrow it down.
Ravensight->TrackProgression(TEXT("complete"), TEXT("world1"), TEXT("level2"), TEXT(""),
    true, 1500.0, {}, TEXT(""));

// An error. Severity is debug, info, warning, error or critical.
Ravensight->TrackError(TEXT("warning"), TEXT("Texture streaming pool is over budget"), TEXT(""), {}, TEXT(""));
```

An amount you hold as a digit string goes through `TrackPurchaseText`. The
currency is upper cased, `Message` is cut to 200 characters and `Stack` to
2000. The helpers do not validate money beyond passing it through: a wrong
amount becomes a flagged event, visible on the Implementation health card,
and the helper never throws. In Blueprint the same calls are under
**Ravensight > Typed Events**.

#### Performance sampling

Turn on **Performance** in Project Settings to send one `health` event per
session. Once a second the frame rate is sampled (a reservoir of at most 600
samples is kept). When the app goes to the background on iOS and Android, or on
quit, the SDK sends the median and 10th percentile frames per second
(`rs_fps_median`, `rs_fps_p10`), memory in MB (`rs_memory_mb`) and the time from
process start to `Init` in milliseconds (`rs_boot_ms`). It is never sent per
frame, never more than once per session, and nothing is sent when no sample was
taken. It is off by default. The session create request also carries
`"sdk": "unreal/<version>"`.

## API

Everything lives on `URavensightSubsystem` and everything below is Blueprint
callable.

| Call | What it does |
| --- | --- |
| `Get(WorldContextObject)` | Static accessor. **Get Ravensight** in Blueprint. Null outside a live game instance. |
| `Init(IngestKey, ApiUrl, GameVersion)` | Starts the SDK. Only needed with Auto Initialize off. Later calls are ignored. |
| `TrackEvent(EventName, Data)` | Queues an event with string valued properties. Only queues: see [How delivery works](#how-delivery-works). |
| `TrackEventJson(EventName, JsonData)` | Queues an event with a JSON object payload, for numbers, bools, arrays or nesting. |
| `TrackPurchase(AmountMinor, Currency, ItemType, ItemId, Cart, TransactionId, Extra, EventName)` | Typed purchase. Integer minor units, currency upper cased. Default name `purchase`. See [Typed events](#6-typed-events). |
| `TrackPurchaseText(AmountMinor, Currency, ItemType, ItemId, Cart, TransactionId, Extra, EventName)` | The same purchase for a digit string amount, sent as a string. |
| `TrackResource(Flow, Resource, Amount, ItemType, ItemId, Extra, EventName)` | Typed resource flow, `source` or `sink`. Default name `resource_source` or `resource_sink`. |
| `TrackProgression(Status, P1, P2, P3, bHasScore, Score, Extra, EventName)` | Typed progression, `start`, `complete` or `fail`. Default name `progression_<status>`. |
| `TrackError(Severity, Message, Stack, Extra, EventName)` | Typed error. Message cut to 200 characters, stack to 2000. Default name `error`. |
| `Flush()` | Sends queued events now, ignoring the retry window. After a refused key, the one way to make another attempt. |
| `SubmitFeedback(Message, FeedbackCategory, Rating)` | Posts player feedback. Category is empty or one of `bug`, `suggestion`, `complaint`, `praise`, `playtest`, `other`; Rating is 1 to 5, or 0 to omit. A bad category or rating fails locally with `invalid_category` or `invalid_rating` and sends nothing. While tracking is off it fails with `disabled`. Needs an open session (`no_session` otherwise). |
| `GetFeedbackCategories()` | The six categories above. |
| `FetchSuggestions()` | EXPERIMENTAL. AI generated design suggestions for your game. While tracking is off, answers at once with a failure and an empty list, and makes no request. |
| `SetTrackingEnabled(bool)` | The player's opt in and opt out. Saved to `GameUserSettings.ini` and read back on every launch. |
| `SaveTrackingEnabled(bool)` / `LoadTrackingEnabled()` | Static: write or read the saved choice without a running subsystem. |
| `SetEnabled(bool)` | The developer switch, the runtime side of the Enabled setting. Not saved. |
| `ResetDeviceId()` | Forgets this install's identity: a new saved id, an empty queue, a new session on the next event. Returns the new id. |
| `SetPlaytestContext(Token, RunId, JobId, Persona)` | Marks the run as a Playtest run, for platforms where the runner cannot set the environment. Call before `Init`. |
| `IsSessionReady()` | True once a session token exists and has not expired. |
| `IsTrackingActive()` | True when the Enabled setting, the kill switch, the player's choice and the key all allow sending. |
| `IsEnabled()` | True only while the plugin is collecting: started with a key and not switched off by the Enabled setting, the player, the server or a refused key. False before `Init` and while inert. |
| `GetDistribution()` | The distribution label sent on the session, empty when none is sent. |
| `GetStatusReason()` | Why the SDK is not ready, or empty when it is. See [Status reasons](#status-reasons). |
| `GetSessionToken()` | The current session token, empty until a session opens. |
| `IsPlaytest()` | True during a Ravensight Playtest run. |
| `GetQueuedEventCount()` | Events waiting to be sent. |
| `GetDeviceId()` | The device identifier this install reports. Empty before `Init`, while inert, and with Consent on Opt In before the first yes. |

### Delegates

Every one is a `BlueprintAssignable` dynamic multicast, so **Assign** works on
all of them.

| Delegate | Fires when |
| --- | --- |
| `OnSessionReady` | A session token has been issued and events can go out. |
| `OnSessionFailed(Reason)` | Session creation failed. The SDK retries on its own, except after a refused key. |
| `OnTrackingDisabled(Source)` | Once, when tracking stops for the rest of the run. Source is `server` for the kill switch (from `GET /settings` or a `403 tracking_disabled`), `playtest_job_closed` when a Playtest job has ended, or `key` when the settings check or `POST /session` refused the ingest key. |
| `OnStatusChanged(Reason)` | The status reason changed. Empty means ready. |
| `OnEventsFlushed(Count)` | A batch was accepted by the server. |
| `OnFlushFailed(Reason)` | A batch flush failed. Informational: the retry is automatic. `event_too_large`: one event's data was over 8,192 bytes as JSON and it was dropped before it was queued, never sent. |
| `OnFeedbackResult(bSuccess, Reason)` | A feedback submission finished. Reason is empty on success. |
| `OnSuggestionsReceived(bSuccess, Suggestions)` | EXPERIMENTAL. Each entry is one suggestion as a JSON object string. |

Suggestions come back as strings rather than a `USTRUCT` on purpose: the server
side shape is still evolving, and a struct would break on the next change.

### Status reasons

`GetStatusReason()` is empty when the SDK is ready (or has not started), and
otherwise one of the values every Ravensight SDK uses:

| Reason | Meaning |
| --- | --- |
| `no_key` | Started with no ingest key. Inert: nothing is stored or sent until an `Init` with a key. Logged once unless Optional is ticked. |
| `disabled` | The Enabled setting is off, or the player opted out (or, with Consent on Opt In, has not said yes yet). Nothing is sent. |
| `invalid_key` | The ingest key was refused. On the settings check or `POST /session` (a `401`, or a `403` other than the three below) the run ends: the queue is cleared, nothing more is sent, and `OnTrackingDisabled` fires with `key`. On a later batch `403`, automatic retries stop, events keep queueing up to the cap, and `Flush()` makes one more attempt. Logged once as a warning: `Ravensight: ingest key refused (invalid_key), sending stopped for this run. Check your gt_live_ or gt_pub_ key.` |
| `tracking_disabled` | The kill switch is on for your game. The queue is cleared and nothing more is sent this run. |
| `offline` | The server could not be reached. Retried from 10 seconds, doubling to 5 minutes, each wait plus or minus 20 percent. |
| `rate_limited` | A `429`. Retried after exactly `Retry-After` seconds. |
| `session_failed` | A session could not be opened for another reason, or a Playtest token was refused or arrived without a run id. |
| `playtest_job_closed` | The Playtest job for this run has ended. Nothing more is sent this run. |

### Handing the session to a game server

`GetSessionToken()` (**Get Session Token** in Blueprint) is the open session's
token, empty until one opens. Pass it to your game server as `joinToken` so
the server's events join this player's session.

## Project settings

| Setting | Default | Meaning |
| --- | --- | --- |
| Api Url | `https://api.ravensight.io` | Only change this if you self host. Whitespace and trailing slashes are trimmed, a trailing `/api/v1` is accepted, and `/api/v1` is appended. Empty means the default. |
| Ingest Key | none | Your publishable `gt_live_...` key, or a public build's `gt_pub_...` key. Empty makes the plugin inert. |
| Optional | off | Silences the one warning an inert start (no ingest key) logs. |
| Distribution | empty | A short label sent on the session, such as `native-release`. At most 40 characters, no control characters. Empty sends none. |
| Game Version | empty | Empty uses the project version from Project Settings. |
| Enabled | on | Off means no request of any kind until `SetEnabled(true)`. Not saved between runs, and separate from the player's own choice. |
| Auto Initialize | on | Off waits for an explicit `Init` call. |
| Consent | Opt Out | Opt Out: on until the player opts out. Opt In: nothing is sent and no device id is made until `SetTrackingEnabled(true)`. |
| Flush Interval Seconds | `5` | How often queued events are sent. `0` sends only at 50 queued, on `Flush`, on background and on quit. |
| Max Queue Size | `500` | Queue cap. Oldest events are dropped first. |
| Track Lifecycle Events | on | Sends `game_started` once per session and `game_exited` once on quit. |
| Track Pause Events | off | Also sends `game_paused` on entering the background and `game_resumed` on return. |
| Session Timeout Seconds | `1800` | iOS and Android: a background stay at least this long ends the session. |
| Request Timeout Seconds | `15` | Per request timeout. |
| Performance | off | Samples the frame rate once a second and sends one `health` event per session, on background (mobile) or quit. See [Performance sampling](#performance-sampling). |
| Verbose Logging | off | Logs every session, batch and retry decision to `LogRavensight`. |

Watch the SDK work with `Log LogRavensight Verbose` in the console, or turn
Verbose Logging on to get the same lines at normal verbosity in a packaged
build.

## Public builds

For a game anyone can download, fork or play: a demo, a game jam entry, an
open source project. A project that sets only its key behaves as on 0.2.1,
apart from the refused key and jitter changes listed in the CHANGELOG.

* **Use a public key, injected at build time.** Create a public `gt_pub_...`
  key for the game in the dashboard and never commit it. Leave **Ingest Key**
  empty in the committed `Config/DefaultGame.ini`, and have your release build
  supply it, for example by writing it into the staged copy of the config in
  the build job only, or by turning **Auto Initialize** off and calling
  `Init` with a value from a header your build script generates and your
  `.gitignore` lists:

  ```cpp
  // Source/YourGame/Generated/RavensightBuildKey.h, written by the build script, gitignored
  #define RAVENSIGHT_BUILD_KEY TEXT("gt_pub_...")

  Ravensight->Init(RAVENSIGHT_BUILD_KEY);
  ```

  Ship a placeholder header with an empty key in the repository so a clone
  still compiles.
* **No key, no analytics.** With an empty key the plugin is inert: it writes
  nothing to `GameUserSettings.ini`, makes no request, starts no timer, and
  every call is a quiet no-op. `GetStatusReason()` is `no_key`. It logs one
  warning; tick **Optional** to silence it. So a clone or a development build
  sends nothing. A later `Init` with a key still starts it.
* **`IsEnabled()`** is true only while the plugin is collecting. Use it to
  skip work that only feeds analytics.
* **Consent: Opt In** sends nothing until the player says yes. No device id is
  made and not even the settings check is sent before
  `SetTrackingEnabled(true)`. Events tracked before the yes are dropped, not
  held for later; events tracked right after it are queued while the settings
  check runs. The yes is saved as `[Ravensight] OptIn` in
  `GameUserSettings.ini`, beside the existing `TrackingEnabled`, and
  `SetTrackingEnabled(false)` removes it. Saying no again stops everything but
  keeps the device id; `ResetDeviceId()` forgets it. `LoadTrackingEnabled()`
  reports the choice for the project's Consent setting, also before `Init`,
  so the settings checkbox always shows the right state. The default,
  **Opt Out**, keeps the 0.2.1 behaviour.
* **Distribution** is a short label of your own, such as `native-release`,
  `web-demo` or `jam-build`, sent on the session as `distribution` so you can
  tell builds apart. At most 40 characters and no control characters; a label
  that breaks either rule is not sent and is logged once. Empty sends none.
* **`rs_event_id`** is added to every event's data: the first 16 characters of
  the device id (characters outside `A-Z`, `a-z`, `0-9`, `_` and `-` become
  `_`), the start time in hex, an 8 hex character value made once per launch
  from the platform's GUID generator (`FGuid::NewGuid`, backed by the system's
  secure random source), and a counter. It always matches
  `^[A-Za-z0-9_-]{1,64}$`. Events tracked before `Init` get the device part
  when `Init` runs. A batch sent again after a failure carries the same ids,
  and the server drops a repeat. The queue lives in memory, so events still
  queued when the game exits are not sent on the next launch.
* **A refused key ends the run.** A `401` or `403` to the settings check or to
  opening a session (other than `tracking_disabled` and a refused Playtest
  token) means the key was revoked, rotated or is wrong, and a retry cannot
  help. The plugin stops for the rest of the run: queue cleared, timers
  stopped, nothing more sent, one warning, `OnTrackingDisabled("key")`. A
  later `SetTrackingEnabled(true)` saves the choice but restarts nothing; the
  next launch tries the key again. A `401` on a batch still means the session
  expired: a new one is opened first.
* **Jitter.** A failed request is retried after the backoff plus or minus 20
  percent, never sooner than half a second and never later than 5 minutes. A
  server's `Retry-After` is obeyed exactly.
* **Events over 8,192 bytes** of data as JSON (what the server checks; the
  name and timestamp are not counted) are dropped before they are queued,
  with one warning for the run and `OnFlushFailed("event_too_large")` for
  each.

What a request carries: the settings check sends only the `X-API-Key`
header; the session sends `deviceId`, `gameVersion`, `platform` and, when
set, `distribution`; a batch sends `events`, each with `event`, `data` and
`timestamp`; feedback sends `message` and the optional `category` and
`rating`.

## About your ingest key

The `gt_live_...` key is publishable. It is safe to ship inside a packaged
build: it can open sessions, read the tracking kill switch and read this
game's design suggestions. It cannot read analytics, read feedback or touch
your account. Rotate it from the dashboard at any time.

## How delivery works

* **Batching.** `TrackEvent` only queues. Queued events are sent every Flush
  Interval Seconds (5 by default), straight away once 50 are waiting, on
  `Flush`, on `ApplicationWillDeactivate` and `ApplicationWillEnterBackground`,
  on `OnWorldBeginTearDown` (the subsystem's stand-in for an actor's
  `EndPlay`) and on quit. Never one request per event. Up to 50 events go in
  a request, the server limit, and a flush keeps sending until the queue
  drains.
* **Queue.** Events accumulate up to Max Queue Size. Past that the oldest are
  dropped so the newest are kept. The queue lives in memory only.
* **Failures keep events.** Offline, a timeout or a server error keeps every
  event and retries from 10 seconds, doubling to a 5 minute ceiling, each wait
  plus or minus 20 percent (never under half a second) so many players who
  lost the network together do not come back together. A `429`, or a `503`
  that sends `Retry-After`, waits exactly that many seconds.
* **Event ids.** Every event's data carries an `rs_event_id`, so a batch sent
  again after a failure is recognised as the same events.
* **Oversized events.** An event whose data is over 8,192 bytes as JSON is
  dropped before it is queued, with one warning for the run and
  `OnFlushFailed("event_too_large")` for each.
* **A batch the server refuses.** A `400` on a batch halves it and retries; a
  single event that still gets a `400` (too large, a bad name) is dropped,
  counted in `GetQueue().GetStats().Dropped`, logged once, and the rest of the
  queue keeps draining.
* **Session expiry.** A `401` on a batch clears the session token, leaves the
  events in place, opens a new session and re sends the same batch, at most
  twice in a row before backing off.
* **Kill switch.** `GET /settings` is read once at start, and a `403
  tracking_disabled` from any request means the same. The queue is cleared,
  the timers stop and nothing is sent for the rest of that run. Flip it back
  from the dashboard and it takes effect on the next launch, no update
  required.
* **Every request** times out after Request Timeout Seconds (15).

The quit flush is best effort: `game_exited` is queued once, then the quit is
held for at most 1.5 seconds while the HTTP manager is pumped by hand for the
last session and batch requests. Anything not delivered by then is lost. If
the final events matter to you, call `Flush` at a point you control, such as
the end of a level.

## Automatic events

With Track Lifecycle Events on (the default) the plugin sends exactly two
events by itself:

* `game_started`, queued once per session when the SDK starts, so it is first
  in the queue and carries the real start time. It is never queued while the
  Enabled setting is off or the player has opted out; if tracking is turned
  on later in the run, it is queued then, once.
* `game_exited`, queued once when the game quits (`Deinitialize`), followed by
  the bounded flush above.

Desktop focus loss is not an exit and sends nothing, though queued events are
flushed. `game_paused` and `game_resumed` are only sent with Track Pause
Events on.

**iOS and Android.** Entering the background flushes and saves a small marker
(the session token, its expiry and the time) to `GameUserSettings.ini`. No
event is sent. On return the marker is deleted; if the app was away for
Session Timeout Seconds (30 minutes) or more, `game_exited` is sent under the
old session with the time the app left, and a new session opens with
`game_started`. Under 30 minutes nothing is sent. If the app was swiped away
in the background, the next launch finds the marker and, unless the player has
opted out or the token has expired, sends that one `game_exited` before
anything else, then deletes the marker whatever the answer.

## Playtest runs

When the Ravensight Playtest runner starts your build it sets
`RAVENSIGHT_PLAYTEST_TOKEN` and `RAVENSIGHT_PLAYTEST_RUN_ID` (desktop builds
also accept `--ravensight-playtest-token=` and `--ravensight-playtest-run-id=`
on the command line). The plugin then reports as `pt-<runId>`, kept in memory
and never saved, sends the token in an `X-Ravensight-Playtest` header on every
request, tags each event with the run, and ignores the player's saved opt-out
without changing it. `SetTrackingEnabled` and `ResetDeviceId` do nothing
during a run apart from one log line. A token without a run id never runs as
a real player: nothing is sent and the reason is `session_failed`. When the
job ends the server answers `403 playtest_job_closed`, and the plugin stops
for the rest of the run. Where the runner cannot set the environment, turn
Auto Initialize off and call `SetPlaytestContext` before `Init`. `IsPlaytest()`
says whether this is a Playtest run.

## Architecture

```
Source/Ravensight/
  Public/
    RavensightQueue.h        the protocol state machine, no UObject, no HTTP
    RavensightSettings.h     UDeveloperSettings, Project Settings > Plugins
    RavensightSubsystem.h    UGameInstanceSubsystem, the entry point
    RavensightProtocol.h     response table, feedback rules, lifecycle rules
    RavensightTypedEvents.h  typed event property sets and the health sampler, no UObject
  Private/
    RavensightQueue.cpp
    RavensightProtocol.cpp
    RavensightTypedEvents.cpp
    RavensightSettings.cpp
    RavensightSubsystem.cpp  HTTP, timers, JSON, config, application lifecycle
    RavensightTransport.*    where requests are created and sent; the specs record them here
    RavensightBackgroundTask.h  iOS background task for the background flush, no-op elsewhere
    RavensightModule.cpp
    IOS/
      RavensightIOSBackgroundTask.mm  compiled for iOS only
    Tests/
      RavensightQueue.spec.cpp
      RavensightProtocol.spec.cpp
      RavensightTypedEvents.spec.cpp
      RavensightSubsystem.spec.cpp
      RavensightSpecListener.h
Tools/
  host-check/                runs the engine free specs on a plain C++17 compiler
```

`FRavensightQueue` makes every decision that is easy to get wrong: what goes in
a batch, what leaves the queue and when, how long to wait after each kind of
failure, when a session token counts as expired. It takes the current time as a
parameter rather than reading a clock, so the spec drives a full backoff ladder
or a session expiry without sleeping and without flaking.

`RavensightProtocol` decides what each HTTP answer means: one table maps an
endpoint, a status and the server's `error` code to send, drop the event,
halve the batch, refresh the session, refused key, kill switch, Playtest stop,
back off or wait `Retry-After`.

`URavensightSubsystem` is the part that needs the engine. It owns the HTTP
requests, the timers, JSON encoding, the saved settings and the application
lifecycle hooks, and it asks `FRavensightQueue` and `RavensightProtocol` what to
do at every branch.

## Running the tests

There are three automation specs under `Source/Ravensight/Private/Tests/`:
`Ravensight.Queue`, `Ravensight.Protocol`, and `Ravensight.Subsystem` (5.8 or
newer), which runs the real subsystem with its HTTP requests recorded
instead of sent.

Editor: **Tools > Session Frontend > Automation**, filter for `Ravensight`.

Headless:

```bash
UnrealEditor-Cmd YourProject.uproject \
  -ExecCmds="Automation RunTests Ravensight; Quit" \
  -unattended -nopause -nullrhi -testexit="Automation Test Queue Empty" \
  -ReportExportPath="$PWD/AutomationReport"
```

On Unreal Engine 5.8.3 on macOS: 99 tests, 99 passed (0.3.0).

Without the engine, on any machine with a C++17 compiler:

```bash
bash Tools/host-check/run.sh
```

That builds the queue, the protocol table and their two specs against stand-in
headers and runs them. It checks the engine-free logic only; it is not an
Unreal build. See `Tools/README.md` for compile checking with the engine.

## Device id and privacy

The plugin generates a random GUID on first run and stores it in the local game
user settings ini under `[Ravensight] DeviceId`. It does not use
`FPlatformMisc::GetDeviceId`, an advertising identifier, an IDFA, an Android
advertising id, or any hardware identifier. Nothing about the player or the
device is collected by the SDK itself. Only the events you choose to send leave
the machine.

Because no advertising identifier is involved, an iOS build does not need an App
Tracking Transparency prompt for Ravensight. You do still need to declare a
device identifier used for analytics in your App Store privacy details and your
Apple privacy manifest. On Android the `INTERNET` permission is all that is
required. See the
[platform support](https://ravensight.io/docs/#platform-support) section of the
docs for the full picture.

`SetTrackingEnabled(false)` stops all sending, discards anything still
queued and stops the timer, so an opt out does not leave player data sitting
in memory. The choice is saved straight away under `[Ravensight]
TrackingEnabled` in `GameUserSettings.ini` and read back on every launch
before anything is sent, so an opted out player's machine never contacts
Ravensight, not even to read the kill switch. A saved opt out wins over the
Enabled setting. `SaveTrackingEnabled` writes the same value from code that
runs before the game instance exists.

`ResetDeviceId()` forgets this install's analytics identity: it saves a new
random id, clears the queue and drops the session, so nothing links the old
id to the new one.

From Blueprint: **Get Ravensight**, then **Set Tracking Enabled** with the
checkbox from your options menu, and **Reset Device Id** behind a "Forget me"
button. From C++:

```cpp
void UMyOptionsMenu::OnAnalyticsToggled(bool bOn)
{
    if (URavensightSubsystem* Ravensight = URavensightSubsystem::Get(this))
    {
        Ravensight->SetTrackingEnabled(bOn);   // remembered across launches
    }
}

void UMyOptionsMenu::OnForgetMeClicked()
{
    if (URavensightSubsystem* Ravensight = URavensightSubsystem::Get(this))
    {
        Ravensight->ResetDeviceId();           // a fresh random identity from now on
    }
}
```

The checkbox's starting state is `URavensightSubsystem::LoadTrackingEnabled()`.

A game that sends gameplay events and nothing else can declare Device ID and
Product Interaction as not linked to the player and not used for tracking,
and needs no tracking permission prompt. The row by row answers for the App
Store and Google Play forms are at https://ravensight.io/docs/#privacy-label.

## Telling players

You do not need a permission prompt, but saying what the game sends is good
manners. A paragraph your game can show, in a settings screen or on its
store page:

> This game sends anonymous gameplay events (what happens in the game, never
> who you are) to Ravensight so we can improve it. You can turn this off in
> Settings at any time.

And the toggle behind "turn this off" is the `SetTrackingEnabled` and
`ResetDeviceId` example above: an "Analytics" checkbox and, if you like, a
"Forget me" button.

## Platforms

The plugin is plain HTTPS over the engine's own HTTP module, so it works
wherever your project ships: Windows, macOS, Linux, iOS, Android and console.
Console networking still has to pass platform certification, which is the
developer's responsibility as it would be for any online feature. So far it
has been built and spec tested on macOS only (Unreal Engine 5.8.3); see
[Verification status](#verification-status).

## Other SDKs

* [Godot](https://github.com/Reality-Software-Entertainment/ravensight-godot)
* [LÖVE](https://github.com/Reality-Software-Entertainment/ravensight-love)
* [Unity](https://github.com/Reality-Software-Entertainment/ravensight-unity)
* [JavaScript](https://github.com/Reality-Software-Entertainment/ravensight-js), web and Node

## License

MIT. Copyright 2026 Reality Software Entertainment.

### Player Reports with screenshots (0.3.1)

Use `PrepareReport(ClientId, Message, Category, Screenshot)` after the player previews
and approves an encoded PNG, JPEG or WebP screenshot. Supply bytes from your game's own
capture/encoding flow, at most 1,000,000 bytes. Empty bytes explicitly mean text-only;
this API does not capture the desktop or silently fall back to text.

Keep the returned handle and bind `OnReportResult(Handle, Status, ReportId, Error)`.
Once `OnSessionReady` fires, call `SubmitReport(Handle)`. `processing` means accepted
for sanitization, not yet in the inbox. Explicitly call `GetReportStatus(Handle)` until
`complete` or `canceled`. On a retryable failure, call `SubmitReport` on that SAME
handle. `CancelReport` releases an unfinished submission through the server;
`ForgetReport` only releases local memory after the operation has finished.

Preparation copies the payload/image and capture timestamp, sends no request, and keeps
at most 16 handles. All calls must be on the game thread. The first submission pins the
current session, and later operations do not silently open a new one. Reset identity,
opt-out, shutdown and synthetic playtest sessions prevent submission. Weak callbacks
reject stale responses. Storage uploads use fresh HTTPS multipart requests without
API, session or playtest credentials. No automatic retries or polling occur.

Screenshot reports reserve 20,001 weighted events; text-only reports use 1 event.
Processing and refunds are server-authoritative. A transport error is not proof that
nothing was charged: poll or cancel the retained handle. Server support requires Player
Reports enabled with private storage, migrated Mongo indexes and the processing worker.
The server strips image metadata and delivers sanitized JPEGs and thumbnails to the inbox.
