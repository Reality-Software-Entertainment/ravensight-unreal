# Ravensight for Unreal Engine

Official Unreal Engine 5 plugin for [Ravensight](https://ravensight.io) player
analytics.

Sessions, batched events, an offline queue, rate limit handling and a server
side kill switch, in one game instance subsystem with no third party
dependencies. Everything is exposed to Blueprint, so a designer can ship it
without writing C++.

> **Not yet compiled by its maintainers.** Version 0.2.0 of this plugin has
> never been built by UnrealBuildTool or run in the Unreal editor, because the
> machine it is written on has no Unreal toolchain. Expect a compile error or
> two on first build. What has been run is listed under
> [Verification status](#verification-status).

* API base: `https://api.ravensight.io` (the plugin appends `/api/v1`)
* Docs: https://ravensight.io/docs/
* Privacy: a random per-install device id, no advertising identifier, a saved player opt-out (`SetTrackingEnabled(false)`), no tracking permission prompt needed. What your store label can say: https://ravensight.io/docs/#privacy-label
* Unreal Engine 5.1 or newer

## Verification status

**Beta. Engine-free logic checked on a host compiler; the plugin itself is not
yet compiled or run in Unreal.**

Every queue, batching, retry and response decision lives in two plain C++
pieces with no UObject, no HTTP and no engine tick: `FRavensightQueue` and the
response table in `RavensightProtocol`. Both have automation specs.

What was run, on 2026-10-10: `bash Tools/host-check/run.sh`, which compiles
`RavensightQueue.cpp`, `RavensightProtocol.cpp` and both `.spec.cpp` files
unchanged with Apple clang 21.0.0 (`-std=c++17 -Wall -Wextra -Wshadow -Werror`)
against small stand-in headers for the few engine types they use, and runs
the specs: 54 cases, 0 failed. That checks the logic, not Unreal.

What has **not** been run: UnrealHeaderTool, UnrealBuildTool, the automation
specs inside the editor, and anything in `URavensightSubsystem` (HTTP, timers,
config, application lifecycle). None of that is verified yet.

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

Pin a release with `cd Plugins/Ravensight && git checkout v0.2.0` once that
tag exists.

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
  `https://api.ravensight.io`), `Ingest Key`, `Game Version`.
* **Behavior**: enabled, auto initialize, flush interval, queue size,
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

## API

Everything lives on `URavensightSubsystem` and everything below is Blueprint
callable.

| Call | What it does |
| --- | --- |
| `Get(WorldContextObject)` | Static accessor. **Get Ravensight** in Blueprint. Null outside a live game instance. |
| `Init(IngestKey, ApiUrl, GameVersion)` | Starts the SDK. Only needed with Auto Initialize off. Later calls are ignored. |
| `TrackEvent(EventName, Data)` | Queues an event with string valued properties. Only queues: see [How delivery works](#how-delivery-works). |
| `TrackEventJson(EventName, JsonData)` | Queues an event with a JSON object payload, for numbers, bools, arrays or nesting. |
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
| `GetStatusReason()` | Why the SDK is not ready, or empty when it is. See [Status reasons](#status-reasons). |
| `GetSessionToken()` | The current session token, empty until a session opens. |
| `IsPlaytest()` | True during a Ravensight Playtest run. |
| `GetQueuedEventCount()` | Events waiting to be sent. |
| `GetDeviceId()` | The device identifier this install reports. |

### Delegates

Every one is a `BlueprintAssignable` dynamic multicast, so **Assign** works on
all of them.

| Delegate | Fires when |
| --- | --- |
| `OnSessionReady` | A session token has been issued and events can go out. |
| `OnSessionFailed(Reason)` | Session creation failed. The SDK retries on its own, except after a refused key. |
| `OnTrackingDisabled(Source)` | Once, when tracking stops for the rest of the run. Source is `server` for the kill switch (from `GET /settings` or a `403 tracking_disabled`), or `playtest_job_closed` when a Playtest job has ended. |
| `OnStatusChanged(Reason)` | The status reason changed. Empty means ready. |
| `OnEventsFlushed(Count)` | A batch was accepted by the server. |
| `OnFlushFailed(Reason)` | A batch flush failed. Informational: the retry is automatic. |
| `OnFeedbackResult(bSuccess, Reason)` | A feedback submission finished. Reason is empty on success. |
| `OnSuggestionsReceived(bSuccess, Suggestions)` | EXPERIMENTAL. Each entry is one suggestion as a JSON object string. |

Suggestions come back as strings rather than a `USTRUCT` on purpose: the server
side shape is still evolving, and a struct would break on the next change.

### Status reasons

`GetStatusReason()` is empty when the SDK is ready (or has not started), and
otherwise one of the values every Ravensight SDK uses:

| Reason | Meaning |
| --- | --- |
| `disabled` | The Enabled setting is off, or the player opted out. Nothing is sent. |
| `invalid_key` | The ingest key was refused (a `401`, or a `403` other than the three below). Automatic retries stop for the run, events keep queueing up to the cap, and `Flush()` makes one more attempt. Logged once as a warning: `Ravensight: ingest key refused (invalid_key), sending stopped for this run. Check your gt_live_ key.` |
| `tracking_disabled` | The kill switch is on for your game. The queue is cleared and nothing more is sent this run. |
| `offline` | The server could not be reached. Retried from 10 seconds, doubling to 5 minutes. |
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
| Ingest Key | none | Your publishable `gt_live_...` key. |
| Game Version | empty | Empty uses the project version from Project Settings. |
| Enabled | on | Off means no request of any kind until `SetEnabled(true)`. Not saved between runs, and separate from the player's own choice. |
| Auto Initialize | on | Off waits for an explicit `Init` call. |
| Flush Interval Seconds | `5` | How often queued events are sent. `0` sends only at 50 queued, on `Flush`, on background and on quit. |
| Max Queue Size | `500` | Queue cap. Oldest events are dropped first. |
| Track Lifecycle Events | on | Sends `game_started` once per session and `game_exited` once on quit. |
| Track Pause Events | off | Also sends `game_paused` on entering the background and `game_resumed` on return. |
| Session Timeout Seconds | `1800` | iOS and Android: a background stay at least this long ends the session. |
| Request Timeout Seconds | `15` | Per request timeout. |
| Verbose Logging | off | Logs every session, batch and retry decision to `LogRavensight`. |

Watch the SDK work with `Log LogRavensight Verbose` in the console, or turn
Verbose Logging on to get the same lines at normal verbosity in a packaged
build.

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
  event and retries from 10 seconds, doubling to a 5 minute ceiling. A `429`,
  or a `503` that sends `Retry-After`, waits exactly that many seconds.
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
  Private/
    RavensightQueue.cpp
    RavensightProtocol.cpp
    RavensightSettings.cpp
    RavensightSubsystem.cpp  HTTP, timers, JSON, config, application lifecycle
    RavensightModule.cpp
    Tests/
      RavensightQueue.spec.cpp
      RavensightProtocol.spec.cpp
Tools/
  host-check/                runs both specs on a plain C++17 compiler
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

There are two automation specs, `Ravensight.Queue` and `Ravensight.Protocol`,
under `Source/Ravensight/Private/Tests/`.

Editor: **Tools > Session Frontend > Automation**, filter for `Ravensight`.

Headless:

```bash
UnrealEditor-Cmd YourProject.uproject \
  -ExecCmds="Automation RunTests Ravensight; Quit" \
  -unattended -nopause -nullrhi -testexit="Automation Test Queue Empty"
```

Neither has been run inside Unreal yet.

Without the engine, on any machine with a C++17 compiler:

```bash
bash Tools/host-check/run.sh
```

That builds the queue, the protocol table and both specs against stand-in
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
developer's responsibility as it would be for any online feature.

## Other SDKs

* [Godot](https://github.com/Reality-Software-Entertainment/ravensight-godot)
* [LÖVE](https://github.com/Reality-Software-Entertainment/ravensight-love)
* [Unity](https://github.com/Reality-Software-Entertainment/ravensight-unity)
* [JavaScript](https://github.com/Reality-Software-Entertainment/ravensight-js), web and Node

## License

MIT. Copyright 2026 Reality Software Entertainment.
