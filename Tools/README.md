# Compile checking this plugin

The plugin was authored on a machine with no Unreal toolchain installed, so it
has never been through UnrealBuildTool. This file is what to run first, and what
to look at when something does not build.

There is no `compile-check.sh` here on purpose. Unreal C++ cannot be compiled
outside the engine: the headers depend on generated `.generated.h` files that
only UnrealHeaderTool produces, so a standalone `clang++` run would fail on the
first include and tell you nothing useful. The checks below are the real ones.

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

## Running the spec

`Source/Ravensight/Private/Tests/RavensightQueue.spec.cpp` covers the whole
state machine and needs no network, no clock and no world.

```bash
UnrealEditor-Cmd YourProject.uproject \
  -ExecCmds="Automation RunTests Ravensight.Queue; Quit" \
  -unattended -nopause -nullrhi
```

The spec is wrapped in `#if WITH_DEV_AUTOMATION_TESTS`, so it compiles out of a
shipping build entirely.

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
| `Ravensight.Build.cs` | The `HTTP` module is spelled in caps. `Http` also resolves on some versions but caps is the safe form. |

## What was verified without a toolchain

* Every brace, bracket and parenthesis balances in all nine source files.
* Every member function declared in a header has exactly one definition, with
  matching return type and `const` qualification, and no definition exists
  without a declaration.
* Every request path, header name, status code and JSON field name was checked
  against `Ravensight.gd`, the reference implementation, field by field.
* No em dashes or en dashes anywhere, per house style.

What that does not cover: template instantiation, UnrealHeaderTool's rules for
Blueprint exposed signatures, and overload resolution against the engine's
actual headers. Those need a real build.
