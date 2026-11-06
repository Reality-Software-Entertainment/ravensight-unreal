// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automatic error capture, engine free: the scrub, the local dedupe key, the
// trail ring, the per session caps and the hold for errors raised before the
// server's answer. The log device and the ensure and Blueprint hooks live in
// the subsystem (Private/RavensightLogCapture.*); they only hand plain
// records to FRavensightErrorPipeline on the game thread.
//
// Like RavensightQueue.h, nothing here touches UObject, JSON, HTTP or the
// engine tick, so RavensightErrorCapture.spec.cpp drives every rule with plain
// values and a clock reading, and Tools/host-check runs it without an engine.
//
// The scrub follows the platform's rules step for step, so text scrubbed here
// passes the server's own scrub unchanged:
//
//   1. a URL loses its query, its fragment and any user:password@
//   2. Bearer tokens, JWT shaped strings and gt_ keys become [token]
//   3. an email address becomes [email]
//   4. /Users/<name>, /home/<name> and C:\Users\<name> become ~
//   5. IPv4 and IPv6 addresses (with a digit) become [ip]
//   6. a run of 32 or more of [A-Za-z0-9+=_-] with a digit becomes [token]
//      ('/' is not in the class, so file paths in stacks survive)

#pragma once

#include "CoreMinimal.h"
#include "RavensightTypedEvents.h"

/** The engine's log verbosities, in the same order, without the engine. */
enum class ERavensightLogLevel : uint8
{
	Fatal,
	Error,
	Warning,
	Display,
	Log,
	Verbose,
	VeryVerbose,
};

/** What the pipeline does with one log line. */
enum class ERavensightLineAction : uint8
{
	/** Not kept at all. */
	Ignore,

	/** Kept in the trail ring only. */
	Trail,

	/** Captured as an error, and kept in the trail ring. */
	Capture,
};

/** One automatic capture, before it is scrubbed, cut and checked against the caps. */
struct RAVENSIGHT_API FRavensightCapturedError
{
	/** warning (an engine log error or an ensure), error (an uncaught Blueprint exception) or critical (Fatal). */
	FString Severity;
	FString Message;
	FString Stack;

	/** log or uncaught. */
	FString Source;

	/** The log category, "Ensure" or the Blueprint exception type. */
	FString ErrorType;
};

namespace RavensightErrors
{
	inline constexpr int32 MaxCaptures = 30;
	inline constexpr int32 MaxPerKey = 3;
	inline constexpr int32 MaxTrails = 10;
	inline constexpr int32 BucketSize = 6;
	inline constexpr double BucketSeconds = 60.0;
	inline constexpr int32 MaxHeld = 5;

	inline constexpr int32 TrailLines = 20;
	inline constexpr int32 TrailLineChars = 120;
	inline constexpr int32 TrailChars = 1500;
	inline constexpr int32 MessageChars = 200;
	inline constexpr int32 StackChars = 2000;
	inline constexpr int32 TypeChars = 100;

	/** The event name every automatic capture uses. */
	inline constexpr const TCHAR* EventName = TEXT("error");

	inline constexpr const TCHAR* SourceLog = TEXT("log");
	inline constexpr const TCHAR* SourceUncaught = TEXT("uncaught");

	/** The contract's scrub. Pure and idempotent. */
	RAVENSIGHT_API FString Scrub(const FString& Text);

	/**
	 * The message with what varies between occurrences taken out (quoted
	 * text, URLs, UUIDs, hex runs of 8 or more, numbers), for the local dedupe
	 * key only. The server builds its own fingerprint.
	 */
	RAVENSIGHT_API FString Template(const FString& Message);

	/**
	 * The first stack line that is not the engine's assertion machinery or
	 * this plugin, trimmed, or empty when there is none.
	 */
	RAVENSIGHT_API FString FirstFrame(const FString& Stack);

	/** The first Count characters. */
	RAVENSIGHT_API FString Cut(const FString& Text, int32 Count);

	/** Carriage returns, new lines and tabs become single spaces. */
	RAVENSIGHT_API FString OneLine(const FString& Text);

	/** The trail's word for a level: critical, error, warning, info or debug. */
	RAVENSIGHT_API FString LevelWord(ERavensightLogLevel Level);

	/**
	 * Fatal and Error capture; Warning always goes to the trail; Display and
	 * Log go to the trail only when the trail is on; Verbose and VeryVerbose
	 * are never kept.
	 */
	RAVENSIGHT_API ERavensightLineAction ClassifyLine(ERavensightLogLevel Level, bool bTrailOn);

	/**
	 * The event for one admitted capture: rs_kind, rs_severity, rs_message,
	 * rs_source, and rs_stack, rs_error_type and rs_trail when not empty.
	 * Message, stack and type are scrubbed and cut here.
	 */
	RAVENSIGHT_API FRavensightTypedEvent BuildEvent(const FRavensightCapturedError& Error, const FString& Trail);
}

/** The last 20 lines, in memory only, oldest first. */
class RAVENSIGHT_API FRavensightTrailRing
{
public:
	/** Scrubs, flattens and cuts one line, "+<ms> <level> <text>", and drops the oldest past 20. */
	void Add(double MsSinceStart, ERavensightLogLevel Level, const FString& Text);

	/** The lines joined by "\n", oldest dropped until it fits 1,500 characters. Empty when none. */
	FString Join() const;

	int32 Num() const { return Lines.Num(); }
	void Reset() { Lines.Reset(); }

private:
	TArray<FString> Lines;
};

/** The per session caps: 30 captures, 3 per dedupe key, 10 trails, a bucket of 6 a minute. */
class RAVENSIGHT_API FRavensightErrorLimiter
{
public:
	/** True when one more capture with this key may go out now; counts it. */
	bool TryAdmit(const FString& DedupeKey, double NowSeconds);

	/** True when one more trail may be attached; counts it. */
	bool TryClaimTrail();

	/** A new session: every count back to zero and a full bucket. */
	void Reset(double NowSeconds);

	int32 GetCaptures() const { return Captures; }
	int32 GetTrails() const { return Trails; }

private:
	int32 Captures = 0;
	int32 Trails = 0;
	TMap<FString, int32> PerKey;
	double Tokens = RavensightErrors::BucketSize;
	double RefilledAt = 0.0;
	bool bBucketStarted = false;
};

/** Whether the server has said capture is on, off, or nothing yet. */
enum class ERavensightCaptureAnswer : uint8
{
	Unknown,
	On,
	Off,
};

/**
 * The whole device side of capture, fed on the game thread. Before the
 * server's answer at most five admitted captures are held in memory; On sends
 * them, Off discards them unsent. Everything produced is returned for the
 * caller to queue behind gameplay events.
 */
class RAVENSIGHT_API FRavensightErrorPipeline
{
public:
	/** Starts the session clock for the trail and the caps. */
	void Start(double NowSeconds);

	/** The server's answer. Returns the held captures to send when it is On. */
	TArray<FRavensightTypedEvent> Answer(bool bEnabled, bool bTrail);

	ERavensightCaptureAnswer GetAnswer() const { return CaptureAnswer; }
	bool IsTrailOn() const { return CaptureAnswer == ERavensightCaptureAnswer::On && bTrailOn; }

	/**
	 * One log line. A captured line returns true and fills Out when it is
	 * ready to send now; held or dropped returns false.
	 */
	bool OnLine(ERavensightLogLevel Level, const FString& Category, const FString& Text, double NowSeconds, FRavensightTypedEvent& Out);

	/** One capture from a hook. Same result as OnLine. Error and Fatal lines become warning and critical. */
	bool OnCapture(const FRavensightCapturedError& Error, double NowSeconds, FRavensightTypedEvent& Out);

	/** A new session or identity: caps, trail and anything held start over. The answer is kept. */
	void ResetForNewSession(double NowSeconds);

	int32 NumHeld() const { return Held.Num(); }
	const FRavensightErrorLimiter& GetLimiter() const { return Limiter; }

	/** The local dedupe key: type, message template and first frame. */
	static FString DedupeKey(const FRavensightCapturedError& Error);

private:
	struct FHeld
	{
		FRavensightCapturedError Error;
		FString Trail;
	};

	FRavensightTrailRing Ring;
	FRavensightErrorLimiter Limiter;
	TArray<FHeld> Held;
	ERavensightCaptureAnswer CaptureAnswer = ERavensightCaptureAnswer::Unknown;
	bool bTrailOn = false;
	double StartedAt = 0.0;
};
