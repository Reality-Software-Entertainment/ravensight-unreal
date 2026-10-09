# Ravensight for Unreal Engine

Official Unreal Engine 5 plugin for [Ravensight](https://ravensight.io) player
analytics.

Sessions, batched events, an offline queue, rate limit handling and a server
side kill switch, in one game instance subsystem with no third party
dependencies. Everything is exposed to Blueprint, so a designer can ship it
without writing C++.

* API base: `https://api.ravensight.io/api/v1`
* Docs: https://ravensight.io/docs/
* Privacy: a random per-install device id, no advertising identifier, a local opt-out (`SetTrackingEnabled(false)`), no tracking permission prompt needed. What your store label can say: https://ravensight.io/docs/#privacy-label
* Unreal Engine 5.1 or newer

## Verification status

**API verified semantics, pending in-editor compile and
run.**

The protocol logic is written and reviewed line by line against the live API
contract, and the part that makes every queue, batching and retry decision
lives in `FRavensightQueue`, a plain C++ class with no UObject, no HTTP and no
engine tick. That class is covered by an automation spec.

What has **not** happened: the plugin has never been compiled by UnrealBuildTool
or run inside the editor, because the machine it was authored on has no Unreal
toolchain installed. Expect to hit a compile error or two on first build, most
likely around an engine API that moved between 5.x versions.

Read `Tools/README.md` before you build. It lists the exact commands to compile
check the plugin and run the spec, and the specific lines most likely to need a
nudge for your engine version. Please open an issue with anything you hit. The
JavaScript and Godot SDKs speak the same protocol and are further along.

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

Pin a release with `cd Plugins/Ravensight && git checkout v0.1.0`.

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
  `https://api.ravensight.io/api/v1`), `Ingest Key`, `Game Version`.
* **Behavior**: auto initialize, flush interval, queue size, lifecycle events,
  request timeout, plus a **Diagnostics** section with verbose logging.

Paste your publishable ingest key (`gt_live_...`) into **Ingest Key**. Leave
**Game Version** empty to use the project version from
**Project Settings > Project > Description**.

That is the whole setup. The values are written to `Config/DefaultGame.ini`, so
they travel with the project and ship in packaged builds. Nothing else is
required: on the next Play In Editor the subsystem reads the kill switch, opens
a session and starts flushing on a timer.

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

Turn **Auto Initialize** off in Project Settings, then start the SDK yourself
once the player has answered:

```cpp
Ravensight->Init(TEXT("gt_live_your_key"));
```

`Init` is also **Init** in Blueprint, with optional Api Url and Game Version
pins that fall back to the project settings when left empty.

## API

Everything lives on `URavensightSubsystem` and everything below is Blueprint
callable.

| Call | What it does |
| --- | --- |
| `Get(WorldContextObject)` | Static accessor. **Get Ravensight** in Blueprint. Null outside a live game instance. |
| `Init(IngestKey, ApiUrl, GameVersion)` | Starts the SDK. Only needed with Auto Initialize off. Later calls are ignored. |
| `TrackEvent(EventName, Data)` | Queues an event with string valued properties. |
| `TrackEventJson(EventName, JsonData)` | Queues an event with a JSON object payload, for numbers, bools, arrays or nesting. |
| `Flush()` | Sends queued events now, ignoring the retry window. |
| `SubmitFeedback(Message, FeedbackCategory, Rating)` | Posts player feedback. Rating is 1 to 5, or 0 to omit. |
| `FetchSuggestions()` | EXPERIMENTAL. AI generated design suggestions for your game. |
| `SetTrackingEnabled(bool)` | Local opt in and opt out for a privacy toggle. |
| `IsSessionReady()` | True once a session token exists and has not expired. |
| `IsTrackingActive()` | True when both the kill switch and the local toggle allow tracking. |
| `GetQueuedEventCount()` | Events waiting to be sent. |
| `GetDeviceId()` | The generated device identifier this install reports. |

### Delegates

Every one is a `BlueprintAssignable` dynamic multicast, so **Assign** works on
all of them.

| Delegate | Fires when |
| --- | --- |
| `OnSessionReady` | A session token has been issued and events can go out. |
| `OnSessionFailed(Reason)` | Session creation failed. The SDK retries on its own. |
| `OnTrackingDisabled(Source)` | Once, at boot, if the server kill switch has tracking off. Source is `server`. |
| `OnEventsFlushed(Count)` | A batch was accepted by the server. |
| `OnFlushFailed(Reason)` | A batch flush failed. Informational: the retry is automatic. |
| `OnFeedbackResult(bSuccess, Reason)` | A feedback submission finished. Reason is empty on success. |
| `OnSuggestionsReceived(bSuccess, Suggestions)` | EXPERIMENTAL. Each entry is one suggestion as a JSON object string. |

Suggestions come back as strings rather than a `USTRUCT` on purpose: the server
side shape is still evolving, and a struct would break on the next change.

## Project settings

| Setting | Default | Meaning |
| --- | --- | --- |
| Api Url | `https://api.ravensight.io/api/v1` | Only change this if you self host. `/api/v1` is appended when omitted. |
| Ingest Key | none | Your publishable `gt_live_...` key. |
| Game Version | empty | Empty uses the project version from Project Settings. |
| Auto Initialize | on | Off waits for an explicit `Init` call, for a consent prompt. |
| Flush Interval Seconds | `5` | `0` flushes only on demand. |
| Max Queue Size | `500` | Offline queue cap. Oldest events are dropped first. |
| Track Lifecycle Events | on | Sends `game_started`, `game_paused`, `game_resumed`, `game_exited`. |
| Request Timeout Seconds | `15` | Per request timeout. |
| Verbose Logging | off | Logs every session, batch and retry decision to `LogRavensight`. |

Watch the SDK work with `Log LogRavensight Verbose` in the console, or turn
Verbose Logging on to get the same lines at normal verbosity in a packaged
build.

## About your ingest key

The `gt_live_...` key is publishable. It is safe to ship inside a packaged
build: it can only open sessions and read the tracking kill switch. It cannot
read analytics, read feedback or touch your account. Rotate it from the
dashboard at any time.

## How delivery works

* **Batching.** Up to 50 events per request, the server hard limit. A flush
  keeps sending batches until the queue drains.
* **Offline queue.** Events accumulate up to Max Queue Size. Past that the
  oldest are dropped so the newest are always kept.
* **Never dropped on failure.** Events leave the queue only after the server
  answers `202`.
* **Session expiry.** A `401` clears the session token, leaves every event in
  place, opens a new session and re sends the same batch.
* **Rate limits.** A `429` honors `Retry-After` in seconds. With no such header
  the SDK backs off exponentially from 10 seconds to a 5 minute ceiling. The
  queue is held, never discarded.
* **Kill switch.** `GET /settings` is read once at boot. If the server has
  tracking off for your game, the queue is cleared, the flush timer stops and
  nothing is sent for that run. Flip it back from the dashboard and it takes
  effect on the next launch, no update required.

Flushes happen on the timer, on `ApplicationWillDeactivate` and
`ApplicationWillEnterBackground`, on `OnWorldBeginTearDown`, which is the
subsystem's equivalent of an actor's `EndPlay`, and on `Deinitialize` when the
game instance shuts down.

The shutdown flush is best effort. The engine stops pumping the HTTP manager
shortly after `Deinitialize` on most platforms, so a process that exits
immediately can lose the last batch. Anything not delivered is gone, since the
queue lives in memory only. If the final events matter to you, call `Flush` at a
point you control, such as the end of a level.

## Architecture

```
Source/Ravensight/
  Public/
    RavensightQueue.h        the protocol state machine, no UObject, no HTTP
    RavensightSettings.h     UDeveloperSettings, Project Settings > Plugins
    RavensightSubsystem.h    UGameInstanceSubsystem, the entry point
  Private/
    RavensightQueue.cpp
    RavensightSettings.cpp
    RavensightSubsystem.cpp  HTTP, timers, JSON, application lifecycle
    RavensightModule.cpp
    Tests/
      RavensightQueue.spec.cpp
```

`FRavensightQueue` makes every decision that is easy to get wrong: what goes in
a batch, what leaves the queue and when, how long to wait after each kind of
failure, when a session token counts as expired. It takes the current time as a
parameter rather than reading a clock, so the spec drives a full backoff ladder
or a session expiry without sleeping and without flaking.

`URavensightSubsystem` is the part that needs the engine. It owns the HTTP
requests, the timers, JSON encoding and the application lifecycle hooks, and it
asks `FRavensightQueue` what to do at every branch.

## Running the tests

Editor: **Tools > Session Frontend > Automation**, filter for `Ravensight`.

Headless:

```bash
UnrealEditor-Cmd YourProject.uproject \
  -ExecCmds="Automation RunTests Ravensight.Queue; Quit" \
  -unattended -nopause -nullrhi -testexit="Automation Test Queue Empty"
```

See `Tools/README.md` for compile checking without the editor.

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

`SetTrackingEnabled(false)` stops all sending and discards anything still
queued, so an opt out does not leave player data sitting in memory.

The opt-out is not stored by the SDK: keep the player's choice in your own
settings and call `SetTrackingEnabled(false)` before the first event on every
launch. To forget this install's analytics identity, clear `DeviceId` under `[Ravensight]` in `GameUserSettings.ini`;
the next run starts under a fresh id and nothing links the old one back.

A game that sends gameplay events and nothing else can declare Device ID and
Product Interaction as not linked to the player and not used for tracking,
and needs no tracking permission prompt. The row by row answers for the App
Store and Google Play forms are at https://ravensight.io/docs/#privacy-label.

## Platforms

The plugin is plain HTTPS over the engine's own HTTP module, so it works
wherever your project ships: Windows, macOS, Linux, iOS, Android and console.
Console networking still has to pass platform certification, which is the
developer's responsibility as it would be for any online feature.

## Other SDKs

* [Godot](https://github.com/Reality-Software-Entertainment/ravensight-godot)
* [Unity](https://github.com/Reality-Software-Entertainment/ravensight-unity)
* [JavaScript](https://github.com/Reality-Software-Entertainment/ravensight-js), web and Node

## License

MIT. Copyright 2026 Reality Software Entertainment.
