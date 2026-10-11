// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Pure protocol decisions that sit beside FRavensightQueue: what an HTTP
// answer means, whether feedback is valid, how an API URL is normalised, and
// the lifecycle rules (one game_started, one game_exited, the mobile 30 minute
// rule).
//
// Like RavensightQueue.h, nothing here touches UObject, HTTP or the engine
// tick, so RavensightProtocol.spec.cpp can drive every branch with plain
// values and a clock reading.

#pragma once

#include "CoreMinimal.h"

namespace RavensightVersion
{
	/** The plugin version. Matches VersionName in Ravensight.uplugin. */
	inline constexpr const TCHAR* Sdk = TEXT("0.2.1");
}

/**
 * Why the SDK is not ready, in the same vocabulary as every other Ravensight
 * SDK. An empty string means ready (or not started yet).
 */
namespace RavensightReason
{
	inline constexpr const TCHAR* Disabled = TEXT("disabled");
	inline constexpr const TCHAR* InvalidKey = TEXT("invalid_key");
	inline constexpr const TCHAR* TrackingDisabled = TEXT("tracking_disabled");
	inline constexpr const TCHAR* Offline = TEXT("offline");
	inline constexpr const TCHAR* RateLimited = TEXT("rate_limited");
	inline constexpr const TCHAR* SessionFailed = TEXT("session_failed");
	inline constexpr const TCHAR* PlaytestJobClosed = TEXT("playtest_job_closed");
}

/** Which request an answer belongs to. The same status means different things on different endpoints. */
enum class ERavensightEndpoint : uint8
{
	Settings,
	Session,
	Batch,
	Feedback,
	Suggestions,
};

/** What the subsystem should do with an answer. */
enum class ERavensightResponseAction : uint8
{
	/** 2xx. Carry on. */
	Accept,

	/** 400 on a one event batch: the server will never take it. Drop it and keep draining. */
	DropHead,

	/** 400 on a larger batch: retry with half as many events. */
	HalveBatch,

	/** 401 on a batch or feedback: the session expired. Open a new one. */
	RefreshSession,

	/** The ingest key was refused. Automatic retries stop for the run, the queue is kept. */
	FatalKey,

	/** 403 tracking_disabled: the kill switch. Stop for the run and clear the queue. */
	KillSwitch,

	/** 403 playtest_job_closed or invalid_playtest_token. Stop for the run and clear the queue. */
	PlaytestStop,

	/** Offline, a timeout, a 5xx, or a batch 401 past the re open cap. Back off 10 s doubling to 300 s. */
	BackOff,

	/** 429, or 503 with Retry-After. Wait exactly Retry-After, or back off without one. */
	WaitRetryAfter,

	/** Feedback or suggestions only: the call failed and is reported to the caller, not retried. */
	Fail,
};

/** Everything ClassifyResponse needs to know about one answer. */
struct FRavensightResponseInput
{
	ERavensightEndpoint Endpoint = ERavensightEndpoint::Batch;

	/** HTTP status, or 0 when the connection never completed. */
	int32 Status = 0;

	/** The "error" field of the JSON body, empty when there was none. */
	FString Error;

	/** Batch only: how many events were in the batch that got this answer. */
	int32 BatchSize = 0;

	/** Batch only: 401s in a row since the last 202, counting this one. */
	int32 ConsecutiveBatchUnauthorized = 0;

	/** True when the response carried a usable delta seconds Retry-After header. */
	bool bHasRetryAfter = false;
};

/** The outcome of ClassifyResponse. */
struct FRavensightResponseVerdict
{
	ERavensightResponseAction Action = ERavensightResponseAction::Accept;

	/**
	 * The status reason this answer implies, from RavensightReason, or empty
	 * when the answer does not change the reason (a success, a batch split).
	 */
	FString Reason;
};

namespace RavensightProtocol
{
	/** A batch 401 re opens the session at most this many times in a row before backing off. */
	inline constexpr int32 MaxBatchReauthInARow = 2;

	/** Mobile: a background stay at least this long ends the session. Seconds. */
	inline constexpr double DefaultSessionTimeoutSeconds = 1800.0;

	/** The longest a quit is held for the final flush. Seconds. */
	inline constexpr double ExitFlushBudgetSeconds = 1.5;

	/** Every request is abandoned after this long. Seconds. */
	inline constexpr double DefaultRequestTimeoutSeconds = 15.0;

	/** Maps one HTTP answer to one action. Pure. */
	RAVENSIGHT_API FRavensightResponseVerdict ClassifyResponse(const FRavensightResponseInput& Input);

	/**
	 * Trims whitespace, strips every trailing "/", strips a trailing "/api/v1",
	 * then appends "/api/v1". Empty means https://api.ravensight.io.
	 */
	RAVENSIGHT_API FString NormalizeApiUrl(const FString& InUrl);

	/** The six feedback categories the server accepts. */
	RAVENSIGHT_API const TArray<FString>& FeedbackCategories();

	/**
	 * Local feedback validation. Returns an empty string when the call may go
	 * out, otherwise "empty_message", "invalid_category" or "invalid_rating".
	 * An empty category and a rating of 0 both mean "omitted".
	 */
	RAVENSIGHT_API FString ValidateFeedback(const FString& Message, const FString& Category, int32 Rating);

	/** Mobile: true when a background stay this long ends the session. */
	RAVENSIGHT_API bool ShouldEndSessionAfterBackground(double NowSeconds, double BackgroundedAtSeconds, double SessionTimeoutSeconds);

	/**
	 * Mobile cold launch: true when a background marker left by the last run
	 * should be reported as that run's game_exited.
	 */
	RAVENSIGHT_API bool ShouldReportMarkerExit(double NowSeconds, const FString& MarkerToken, double MarkerExpiresAtSeconds, bool bOptedOut);

	/**
	 * The playtest context a build was started with, from the explicit
	 * values first, then the environment, then the command line. Pure: the
	 * caller reads each source and passes it in.
	 */
	struct FPlaytestSources
	{
		FString ExplicitToken;
		FString ExplicitRunId;
		FString EnvToken;
		FString EnvRunId;
		FString CommandLineToken;
		FString CommandLineRunId;
	};

	struct FPlaytestResolution
	{
		FString Token;
		FString RunId;

		bool IsPlaytest() const { return !Token.IsEmpty(); }

		/** A token without a run id must never run as a real player. */
		bool IsMissingRunId() const { return !Token.IsEmpty() && RunId.IsEmpty(); }

		/** "pt-<runId>", the in memory device id of a playtest run. */
		FString DeviceId() const { return TEXT("pt-") + RunId; }
	};

	RAVENSIGHT_API FPlaytestResolution ResolvePlaytest(const FPlaytestSources& Sources);

	/**
	 * The status reason after a session or batch answer: an Accept clears it
	 * (the SDK is sending again), a verdict with a reason sets it, anything
	 * else leaves the current reason alone.
	 */
	RAVENSIGHT_API FString NextStatusReason(const FString& Current, const FRavensightResponseVerdict& Verdict);
}

/** Everything the start gate needs to decide which requests may go out. */
struct FRavensightGateInput
{
	bool bInitialized = false;

	/** The developer's Enabled setting. */
	bool bEnabled = true;

	/** The player's saved choice. Ignored during a Playtest run. */
	bool bPlayerOptedIn = true;

	bool bPlaytest = false;
	bool bPlaytestMissingRunId = false;

	/** Kill switch or Playtest stop. */
	bool bStoppedForRun = false;

	bool bKeyRefused = false;

	/** An explicit Flush, the one attempt allowed after a refused key. */
	bool bExplicitAttempt = false;
};

/** Which requests the SDK may make right now, and the reason when it may make none. */
struct FRavensightGate
{
	bool bAllowEnqueue = false;
	bool bAllowSettings = false;
	bool bAllowSession = false;
	bool bAllowBatch = false;
	bool bAllowSuggestions = false;
	bool bAllowFeedback = false;

	/** disabled, invalid_key or session_failed when blocked for that reason; empty otherwise. */
	FString Reason;

	bool AllowsAnyRequest() const
	{
		return bAllowSettings || bAllowSession || bAllowBatch || bAllowSuggestions || bAllowFeedback;
	}
};

namespace RavensightProtocol
{
	/** The start gate. Pure: active = enabled, not opted out, not stopped, key not refused. */
	RAVENSIGHT_API FRavensightGate DecideGate(const FRavensightGateInput& Input);
}

/**
 * The automatic lifecycle once-guards. game_started is queued once per
 * session and game_exited once per run, however many times a settings check,
 * an opt in or a quit hook fires.
 */
struct RAVENSIGHT_API FRavensightLifecycleGuards
{
	/** True the first time it is called in a session, false after. */
	bool TryClaimStarted();

	/** True the first time it is called in the run, false after. */
	bool TryClaimExited();

	/** A new session after the mobile 30 minute rule may send its own game_started. */
	void ResetForNewSession();

	bool HasStarted() const { return bStarted; }
	bool HasExited() const { return bExited; }

private:
	bool bStarted = false;
	bool bExited = false;
};
