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
}

/** One queued analytics event. Data is carried as an already serialized JSON object. */
struct FRavensightEvent
{
	/** Event name, for example "level_completed". */
	FString EventName;

	/** A JSON object literal, "{}" when the event carries no payload. */
	FString DataJson;

	/** Unix seconds, captured when the event happened rather than when it is sent. */
	int64 TimestampUnix = 0;

	FRavensightEvent() = default;

	FRavensightEvent(FString InEventName, FString InDataJson, int64 InTimestampUnix)
		: EventName(MoveTemp(InEventName))
		, DataJson(MoveTemp(InDataJson))
		, TimestampUnix(InTimestampUnix)
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

	/** Tracking is off, either by the server kill switch or a local opt out. */
	Disabled,
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
	 * Adds an event, dropping oldest events first if the cap is already met.
	 * Returns how many were dropped to make room, so the caller can log it.
	 * A no-op returning 0 when tracking is off.
	 */
	int32 Enqueue(const FRavensightEvent& Event);

	int32 Num() const { return Events.Num(); }
	bool IsEmpty() const { return Events.Num() == 0; }
	const TArray<FRavensightEvent>& GetEvents() const { return Events; }

	/** Discards everything queued and counts it as dropped. */
	void Clear();

	// --- Enablement --------------------------------------------------------

	/** The server side kill switch from GET /settings. Disabling discards the queue. */
	void SetServerTrackingEnabled(bool bEnabled);
	bool IsServerTrackingEnabled() const { return bServerTrackingEnabled; }

	/** Local opt in and opt out, for a privacy toggle. Disabling discards the queue. */
	void SetLocallyEnabled(bool bEnabled);
	bool IsLocallyEnabled() const { return bLocallyEnabled; }

	/** True only when both the server and the local toggle allow tracking. */
	bool IsActive() const { return bServerTrackingEnabled && bLocallyEnabled; }

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
	 * bForce ignores the retry window, for a flush on shutdown or a manual
	 * Flush() call. It never ignores the session or the kill switch.
	 */
	FRavensightFlushPlan BeginFlush(double NowSeconds, bool bForce);

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

	/** Opens a retry window of the current backoff, then doubles it up to the ceiling. */
	void ScheduleBackoff(double NowSeconds);

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
	/** Removes oldest events until the queue fits the cap. Returns how many went. */
	int32 TrimToCap(int32 Headroom);

	TArray<FRavensightEvent> Events;
	int32 MaxQueueSize = RavensightLimits::DefaultQueueCap;

	bool bServerTrackingEnabled = true;
	bool bLocallyEnabled = true;
	bool bFlushInProgress = false;

	FString SessionToken;
	double SessionExpiresAtSeconds = 0.0;

	double NextAttemptAtSeconds = 0.0;
	double BackoffSeconds = RavensightLimits::DefaultRetrySeconds;

	int32 SentCount = 0;
	int32 DroppedCount = 0;
	int32 BatchCount = 0;
};
