# Compile checking this plugin

The plugin was authored on a machine with no Unreal toolchain installed, so it
has never been through UnrealBuildTool. This file is what to run first, and what
to look at when something does not build.

There is no `compile-check.sh` for the plugin as a whole on purpose. Unreal C++
cannot be compiled outside the engine: the subsystem and settings headers
depend on generated `.generated.h` files that only UnrealHeaderTool produces.
The engine checks below are the real ones.

The one thing that does run without the engine is `host-check/` (see
[Host check](#host-check) below): the queue, the protocol table and their
specs, which use no UObject at all.

## The fast path

Put the plugin in a project's `Plugins` folder, then:

```bash
# macOS
/Users/Shared/Epic\ Games/UE_5.4/Engine/Build/BatchFiles/Mac/Build.sh \
  YourProjectEditor Mac Development -project="$PWD/YourProject.uproject" -waitmutex

# Linux
"$UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" \
  YourProjectEditor Linux Development -project="$PWD/YourProject.uproject" -waitmutex
```

```bat
:: Windows
"C:\Program Files\Epic Games\UE_5.4\Engine\Build\BatchFiles\Build.bat" ^
  YourProjectEditor Win64 Development -project="%CD%\YourProject.uproject" -waitmutex
```

That runs UnrealHeaderTool over the `UCLASS`, `UPROPERTY`, `UFUNCTION` and
`DECLARE_DYNAMIC_MULTICAST_DELEGATE` declarations first, which is where most
first build problems in a plugin like this one show up, and then compiles.

## Building the plugin on its own

To produce a redistributable build and catch anything project specific:

```bash
"$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin \
  -Plugin="$PWD/Ravensight.uplugin" \
  -Package="$PWD/../RavensightPackaged" \
  -TargetPlatforms=Mac+IOS+Android
```

## Running the specs

`Source/Ravensight/Private/Tests/RavensightQueue.spec.cpp` covers the queue
state machine and `RavensightProtocol.spec.cpp` covers the response table,
`ApiUrl` forms, feedback validation, the lifecycle once-guards, the mobile 30
minute rule, the Playtest sources and a fake transport over the queue. Neither
needs a network, a clock or a world.

```bash
UnrealEditor-Cmd YourProject.uproject \
  -ExecCmds="Automation RunTests Ravensight; Quit" \
  -unattended -nopause -nullrhi
```

Both specs are wrapped in `#if WITH_DEV_AUTOMATION_TESTS`, so they compile out
of a shipping build entirely.

## Host check

```bash
bash Tools/host-check/run.sh
```

Compiles `RavensightQueue.cpp`, `RavensightProtocol.cpp` and both spec files,
unchanged, with any C++17 compiler (`CXX` overrides `c++`) and
`-Wall -Wextra -Wshadow -Werror`, against stand-in headers in
`host-check/include/` for the handful of engine types they use (`FString`,
`TArray`, `FMath`, `TUniquePtr` and the spec macros), then runs every case.
Where Unreal's semantics matter the stand-ins copy them: `FString` equality and
`EndsWith` ignore case.

This proves the logic, not the build. The subsystem, UnrealHeaderTool,
UnrealBuildTool and the engine's real headers are not involved.

## Known version sensitive lines

If the build fails, look at these before anything else. Each is a spot where an
engine API moved between 5.x releases.

| File | What to check |
| --- | --- |
| `Tests/RavensightQueue.spec.cpp` | `EAutomationTestFlags::EditorContext \| ClientContext \| EngineFilter`. In 5.5 these became a scoped enum and the combined masks were renamed to the `EAutomationTestFlags_` prefix form. The individual flags used here kept their names, but if your version disagrees, swap the expression for whatever `Misc/AutomationTest.h` in your engine declares. |
| `RavensightSubsystem.h` | `#include "Engine/TimerHandle.h"`. On an older 5.0 install this may need to be `#include "Engine/EngineTypes.h"`. |
| `RavensightSubsystem.cpp` | `Request->SetTimeout(RequestTimeoutSeconds)`. Present since 4.26. If your HTTP module predates it, delete the line: the module wide timeout still applies. |
| `RavensightSubsystem.cpp` | `TryGetStringField` / `TryGetBoolField` / `TryGetNumberField` take an `FStringView` from 5.4 onward and an `FString` before that. `TEXT("...")` binds to both, so this should be fine either way. |
| `RavensightSubsystem.cpp` | `FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)`. If the enumerator is missing, `EGuidFormats::Digits` is equivalent for our purposes. |
| `RavensightSubsystem.cpp` | `FHttpModule::Get().GetHttpManager().Tick(0.0f)` in `PumpUntilIdle`, the bounded quit flush. `FHttpManager::Tick(float)` is the manager's ticker override; if your version makes it inaccessible, `GetHttpManager().Flush(...)` with your engine's flush reason is the alternative, though it is not bounded by our 1.5 seconds. |
| `RavensightSubsystem.cpp` | `FPlatformMisc::GetEnvironmentVariable(const TCHAR*)` returning `FString` (UE5 form), and `FParse::Value(FCommandLine::Get(), ..., FString&, false)` for the Playtest sources. |
| `RavensightSubsystem.cpp` | `GConfig->SetBool` / `GetBool` / `SetString` / `GetString` / `Flush` against `GGameUserSettingsIni`, for the saved opt-out and the mobile background marker. |
| `RavensightProtocol.cpp` | `TArray::ContainsByPredicate` with `FString::Equals(..., ESearchCase::CaseSensitive)`. |
| `Ravensight.Build.cs` | The `HTTP` module is spelled in caps. `Http` also resolves on some versions but caps is the safe form. |

## What was verified without a toolchain

For 0.2.0, on 2026-10-10:

* `bash Tools/host-check/run.sh` with Apple clang 21.0.0: the queue, the
  protocol table and both specs compiled with `-Wshadow -Werror` and ran, 66
  cases, 0 failed. It caught one real defect on the way: a parameter that
  shadowed a new queue member, which the engine's own shadowing check would
  also have refused.
* A script check that every member function declared in
  `RavensightSubsystem.h` has a definition in `RavensightSubsystem.cpp`.
* Every request path, header name, status code and JSON field name was read
  against the server's ingest routes.
* No em dashes or en dashes anywhere, per house style.

What that does not cover: UnrealHeaderTool's rules for Blueprint exposed
signatures, the subsystem against the engine's actual headers, and anything at
run time in the editor or a packaged build. Those need a real build.
