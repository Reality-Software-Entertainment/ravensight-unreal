// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The engine side of automatic error capture: an output device on GLog, the
// ensure hook and the Blueprint script exception hook. All three may fire on
// any thread, so each only copies a small record into a lock protected list;
// the subsystem drains it on the game thread and hands it to
// FRavensightErrorPipeline (RavensightErrorCapture.h), which applies the
// caps, the scrub and the trail.
//
// Nothing here changes what the game logs or how an ensure or a script
// exception behaves: every hook observes, none swallows.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"
#include "RavensightErrorCapture.h"

#include <atomic>

class UObject;
struct FFrame;
struct FBlueprintExceptionInfo;

/** One thing a hook saw, copied off whatever thread it fired on. */
struct FRavensightRawCapture
{
	/** A log line for the pipeline's OnLine, or a finished capture for OnCapture. */
	bool bLine = true;

	ERavensightLogLevel Level = ERavensightLogLevel::Log;
	FString Category;
	FString Text;
	FRavensightCapturedError Error;

	/** FPlatformTime::Seconds() when it happened. */
	double AtSeconds = 0.0;
};

class FRavensightLogCapture final : public FOutputDevice
{
public:
	/** OwnHost is the API host: a line naming it is about the plugin's own requests and is never kept. */
	explicit FRavensightLogCapture(const FString& InOwnHost);
	virtual ~FRavensightLogCapture() override;

	/** Registers on GLog and binds the ensure and script exception hooks. Idempotent. */
	void Install();

	/** Unregisters everything. Idempotent. Pending records are kept for a last Drain. */
	void Uninstall();

	bool IsInstalled() const { return bInstalled; }

	/** Display and Log lines are copied only while the game's trail is on. */
	void SetKeepInfoLines(bool bKeep) { bKeepInfo.store(bKeep); }

	/** Moves everything pending into Out, oldest first. Game thread. */
	void Drain(TArray<FRavensightRawCapture>& Out);

	/** True when the engine has FCoreDelegates::OnEnsureFailed, which carries the message and stack. */
	static bool HasEnsureHook();

	// FOutputDevice. Serialize locks internally, so the redirector may call it
	// from any thread, including several at once, without buffering.
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** Ensure entry point, public for the delegate binding helper. */
	void HandleEnsure(const FString& Expression, const TCHAR* Message, const TCHAR* Combined);

private:
	void Push(FRavensightRawCapture&& Record);
	void HandleScriptException(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info);

	/** At most this many records wait for the next drain; more are dropped. */
	static constexpr int32 MaxPending = 64;

	/** A copied line or stack is cut to this many characters. */
	static constexpr int32 MaxCopyChars = 4000;

	FCriticalSection Lock;
	TArray<FRavensightRawCapture> Pending;

	const FString OwnHost;
	std::atomic<bool> bKeepInfo { false };

	/** True while AddOutputDevice replays the engine's backlog: earlier lines are not this session's. */
	std::atomic<bool> bInstalling { false };

	/** The thread inside an "=== Handled ensure: ===" block, whose lines the ensure hook reports instead. */
	std::atomic<uint32> EnsureThreadId { 0 };

	bool bInstalled = false;
	bool bEnsureHooked = false;

	/** Nothing else had bound OnScriptException: the engine's own stack warning is kept by logging it the same way. */
	bool bScriptWasUnbound = false;

	FDelegateHandle EnsureHandle;
	FDelegateHandle ScriptHandle;
};
