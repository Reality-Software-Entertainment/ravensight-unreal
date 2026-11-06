// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The Ravensight protocol state machine: the offline queue, batching, the
// session validity window and the retry schedule.
//
// Nothing in this header touches UObject, HTTP or the engine tick. Every
// decision is a pure function of the current state plus a caller supplied
// clock reading, which is what lets RavensightQueue.spec.cpp drive every
// retry path without a running game.

#pragma once

#include "CoreMinimal.h"

namespace RavensightLimits
{
	/** Server hard limit on POST /track/batch. */
	inline constexpr int32 MaxBatchSize = 50;

	/** Default cap on unsent events held in memory. Oldest are dropped first. */
	inline constexpr int32 DefaultQueueCap = 500;

	/** First retry delay, and the value backoff resets to after a success. */
	inline constexpr double DefaultRetrySeconds = 10.0;

	/** Exponential backoff ceiling. */
	inline constexpr double MaxBackoffSeconds = 300.0;

	/**
	 * Treat a session as expired slightly early, so a token does not lapse
	 * mid flight between the validity check and the server receiving it.
	 */
	inline constexpr double SessionExpirySkewSeconds = 5.0;

	/** Floor on any scheduled retry, so a zero delay never busy loops. */
	inline constexpr double MinRetrySeconds = 0.5;

	/** Retry waits are the backoff times a random factor from this to 1. */
	inline constexpr double JitterFloor = 0.5;
}

/** One queued analytics event. Data is carried as an already serialized JSON object. */
struct FRavensightEvent
{
	/** Event name, for example "level_completed". */
	FString EventName;

	/** A JSON object literal, "{}" when the event carries no payload. */
	FString DataJson;

	/** Unix milliseconds, captured when the event happened rather than when it is sent. */
	int64 TimestampMs = 0;

	/**
	 * The /session answer's origin for the session this event belongs to, as
	 * a JSON object text exactly as received, or empty when not known yet.
	 * An event sent under a later session carries it.
	 */
	FString OriginJson;

	/** The origin's session id, so a send can tell whether the event is from an earlier session. */
	FString OriginSession;

	/** A typed error event: it queues behind gameplay events and is evicted first. */
	bool bError = false;

	FRavensightEvent() = default;

	FRavensightEvent(FString InEventName, FString InDataJson, int64 InTimestampMs, bool bInError = false)
		: EventName(MoveTemp(InEventName))
		, DataJson(MoveTemp(InDataJson))
		, TimestampMs(InTimestampMs)
		, bError(bInError)
	{
	}
};

/** Why a flush attempt did or did not produce a batch to send. */
enum class ERavensightFlushDecision : uint8
{
	/** A batch is ready in FRavensightFlushPlan::Batch. */
	Send,

	/** Nothing is queued. */
	Empty,

	/** A batch is already in flight. */
	AlreadyFlushing,

	/** Still inside the retry window from an earlier failure. */
	BackingOff,

	/** Events are waiting but there is no valid session token yet. */
	NeedsSession,

	/** Tracking is off: the Enabled setting, the server kill switch or a local opt out. */
	Disabled,

	/** A batch was refused with a 403: the events are kept and sent on the next launch. */
	Refused,
};

/** The outcome of FRavensightQueue::BeginFlush. */
struct FRavensightFlushPlan
{
	ERavensightFlushDecision Decision = ERavensightFlushDecision::Empty;

	/** Populated only when Decision is Send. Never larger than MaxBatchSize. */
	TArray<FRavensightEvent> Batch;

	bool ShouldSend() const
	{
		return Decision == ERavensightFlushDecision::Send;
	}
};

/** Counters, useful for a debug overlay or a test assertion. */
struct FRavensightQueueStats
{
	int32 Queued = 0;
	int32 Sent = 0;
	int32 Dropped = 0;
	int32 Batches = 0;
};

/**
 * The queue and retry state machine. Single threaded by contract: the
 * subsystem drives it from the game thread only, so it takes no locks.
 */
class RAVENSIGHT_API FRavensightQueue
{
public:
	// --- Configuration -----------------------------------------------------

	/** Values below 1 are clamped to 1. Shrinking the cap drops oldest events immediately. */
	void SetMaxQueueSize(int32 InMaxQueueSize);
	int32 GetMaxQueueSize() const { return MaxQueueSize; }

	// --- Enqueue -----------------------------------------------------------

	/**
	 * Adds an event. A gameplay event goes ahead of every queued error event
	 * (never inside a batch in flight). When the cap is met the oldest queued
	 * error goes first, then the oldest event; an incoming error with no
	 * error to evict is itself dropped. Returns how many were dropped, the
	 * incoming one included, so the caller can log it. A no-op returning 0
	 * when tracking is off.
	 */
	int32 Enqueue(const FRavensightEvent& Event);

	/**
	 * Puts events read back from the persisted queue ahead of anything queued
	 * in this run, oldest first, then trims to the cap. Never while a batch is
	 * in flight. Returns how many were added.
	 */
	int32 Restore(const TArray<FRavensightEvent>& Restored);

	/** Gives every queued event that has no origin yet this session's origin. */
	void StampOrigins(const FString& InOriginJson, const FString& InOriginSession);

	/**
	 * Like Enqueue, but places the event at the front, for game_started when
	 * the game tracked events before Init. Never ahead of a batch in flight.
	 */
	int32 EnqueueAtFront(const FRavensightEvent& Event);

	int32 Num() const { return Events.Num(); }

	/** The queued event at Index, oldest first. Index must be in range. */
	const FRavensightEvent& GetAt(int32 Index) const { return Events[Index]; }

	/**
	 * Replaces the data of a queued event that has never been sent, for the
	 * subsystem to give events tracked before Init their device id prefix.
	 * Ignored while a batch is in flight or for an index out of range.
	 */
	void ReplaceDataAt(int32 Index, const FString& InDataJson);
	bool IsEmpty() const { return Events.Num() == 0; }
	const TArray<FRavensightEvent>& GetEvents() const { return Events; }

	/** Discards everything queued and counts it as dropped. */
	void Clear();

	/**
	 * Removes up to Count events from the front of the queue and counts them
	 * as dropped. Used for a single event the server answered 400, which would
	 * otherwise block everything behind it. Returns how many went.
	 */
	int32 DropHead(int32 Count);

	/** Removes Count events from Index, the server having named them as refused for good. */
	int32 DropAt(int32 Index, int32 Count);

	/** After a batch 403: keep every event and send nothing more until the next launch. */
	void SetSendBlocked(bool bBlocked) { bSendBlocked = bBlocked; }
	bool IsSendBlocked() const { return bSendBlocked; }

	// --- Enablement --------------------------------------------------------

	/** The server side kill switch from GET /settings. Disabling discards the queue. */
	void SetServerTrackingEnabled(bool bEnabled);
	bool IsServerTrackingEnabled() const { return bServerTrackingEnabled; }

	/** Local opt in and opt out, for a privacy toggle. Disabling discards the queue. */
	void SetLocallyEnabled(bool bEnabled);
	bool IsLocallyEnabled() const { return bLocallyEnabled; }

	/**
	 * The developer's Enabled setting, never persisted. Disabling discards the
	 * queue. Separate from the player's opt out so neither can undo the other.
	 */
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabledSetting; }

	/** True only when the Enabled setting, the server and the local toggle all allow tracking. */
	bool IsActive() const { return bEnabledSetting && bServerTrackingEnabled && bLocallyEnabled; }

	// --- Session -----------------------------------------------------------

	void SetSession(const FString& InToken, double InExpiresAtUnixSeconds);
	void InvalidateSession();
	bool IsSessionValid(double NowSeconds) const;
	const FString& GetSessionToken() const { return SessionToken; }
	double GetSessionExpiresAt() const { return SessionExpiresAtSeconds; }

	// --- Flush lifecycle ---------------------------------------------------

	/**
	 * Decides whether a batch can go out right now and, if so, marks a flush
	 * in progress and returns a copy of the leading events. Events are not
	 * removed here: they leave the queue only in OnBatchAccepted, so a failed
	 * send never loses them.
	 *
	 * bForce ignores a backoff window, for a flush on shutdown, background or
	 * a manual Flush() call. It never ignores a server Retry-After, the
	 * session or the kill switch.
	 *
	 * MaxCount caps the batch below MaxBatchSize, for retrying a batch the
	 * server answered 400 at half its size. Values below 1 are treated as 1.
	 */
	FRavensightFlushPlan BeginFlush(double NowSeconds, bool bForce, int32 MaxCount = RavensightLimits::MaxBatchSize);

	/** Clears the in flight marker. Safe to call even if no flush was started. */
	void EndFlush();

	bool IsFlushInProgress() const { return bFlushInProgress; }

	// --- Responses ---------------------------------------------------------

	/** HTTP 202. Removes the accepted events and clears any backoff. */
	void OnBatchAccepted(int32 BatchSize);

	/**
	 * HTTP 401. Drops the session token and leaves every event queued, so the
	 * same batch is re sent once a fresh session is issued.
	 */
	void OnUnauthorized();

	/**
	 * HTTP 429. Honors RetryAfterSeconds when the server sent a usable
	 * Retry-After header, otherwise falls back to exponential backoff. Pass a
	 * value <= 0 to mean "no header".
	 */
	void OnRateLimited(double NowSeconds, double RetryAfterSeconds);

	/** Any other failure, including a connection that never completed. */
	void OnTransportFailure(double NowSeconds);

	// --- Backoff -----------------------------------------------------------

	/** Back to the 10 second first delay, with no pending retry window. */
	void ResetBackoff();

	/** True while the retry window is a server Retry-After, which nothing cuts short. */
	bool IsRetryAfterActive() const { return bRetryAfterActive; }

	/**
	 * Opens a retry window of the current backoff times a factor in [0.5, 1)
	 * (never under MinRetrySeconds, never over MaxBackoffSeconds), then
	 * doubles the backoff up to the ceiling. The jitter keeps many copies of a
	 * game that lost the network together from coming back together.
	 */
	void ScheduleBackoff(double NowSeconds);

	/** A source of 0 <= x < 1 for the jitter. Null, the default, uses FMath::FRand. */
	using FRandomSource = double (*)();

	/** Replaces the jitter source. A source that returns 0.5 gives the unjittered ladder. */
	void SetRandomSource(FRandomSource InSource) { RandomSource = InSource; }

	/** The jittered wait for one backoff step. Random is 0 <= x < 1; 0.5 gives three quarters of the step. */
	static double JitteredWaitSeconds(double InBackoffSeconds, double Random);

	/** 0 when a request may go out now, otherwise the wait remaining. */
	double SecondsUntilNextAttempt(double NowSeconds) const;

	/** The delay the next ScheduleBackoff would use. */
	double GetBackoffSeconds() const { return BackoffSeconds; }

	/**
	 * SecondsUntilNextAttempt raised to MinRetrySeconds, for handing straight
	 * to a one shot timer that must not fire with a zero delay.
	 */
	double GetRetryTimerSeconds(double NowSeconds) const;

	// --- Stats -------------------------------------------------------------

	FRavensightQueueStats GetStats() const;

private:
	FRandomSource RandomSource = nullptr;

	/** Removes events until the queue fits the cap, errors first. Returns how many went. */
	int32 TrimToCap(int32 Headroom);

	/** The first queued error outside the batch in flight, or INDEX_NONE. */
	int32 FirstQueuedError() const;

	TArray<FRavensightEvent> Events;
	int32 MaxQueueSize = RavensightLimits::DefaultQueueCap;

	bool bEnabledSetting = true;
	bool bServerTrackingEnabled = true;
	bool bLocallyEnabled = true;
	bool bFlushInProgress = false;
	int32 InFlightCount = 0;
	bool bRetryAfterActive = false;
	bool bSendBlocked = false;

	FString SessionToken;
	double SessionExpiresAtSeconds = 0.0;

	double NextAttemptAtSeconds = 0.0;
	double BackoffSeconds = RavensightLimits::DefaultRetrySeconds;

	int32 SentCount = 0;
	int32 DroppedCount = 0;
	int32 BatchCount = 0;
};
