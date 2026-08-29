// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightQueue.h"

// --- Configuration ----------------------------------------------------------

void FRavensightQueue::SetMaxQueueSize(int32 InMaxQueueSize)
{
	MaxQueueSize = FMath::Max(1, InMaxQueueSize);
	TrimToCap(0);
}

// --- Enqueue ----------------------------------------------------------------

int32 FRavensightQueue::TrimToCap(int32 Headroom)
{
	const int32 Limit = FMath::Max(0, MaxQueueSize - Headroom);

	int32 Dropped = 0;
	while (Events.Num() > Limit)
	{
		// Drop oldest so the newest events always survive: a player who has
		// been offline for an hour is better served by the last minute of
		// their session than by the first.
		Events.RemoveAt(0);
		++Dropped;
	}

	DroppedCount += Dropped;
	return Dropped;
}

int32 FRavensightQueue::Enqueue(const FRavensightEvent& Event)
{
	if (!IsActive())
	{
		return 0;
	}

	const int32 Dropped = TrimToCap(1);
	Events.Add(Event);
	return Dropped;
}

void FRavensightQueue::Clear()
{
	DroppedCount += Events.Num();
	Events.Reset();
}

// --- Enablement -------------------------------------------------------------

void FRavensightQueue::SetServerTrackingEnabled(bool bEnabled)
{
	if (bEnabled == bServerTrackingEnabled)
	{
		return;
	}

	bServerTrackingEnabled = bEnabled;
	if (!bEnabled)
	{
		Clear();
	}
}

void FRavensightQueue::SetLocallyEnabled(bool bEnabled)
{
	if (bEnabled == bLocallyEnabled)
	{
		return;
	}

	bLocallyEnabled = bEnabled;
	if (!bEnabled)
	{
		// An opt out should not leave player data sitting in memory waiting
		// for someone to opt back in.
		Clear();
	}
}

// --- Session ----------------------------------------------------------------

void FRavensightQueue::SetSession(const FString& InToken, double InExpiresAtUnixSeconds)
{
	SessionToken = InToken;
	SessionExpiresAtSeconds = InExpiresAtUnixSeconds;
}

void FRavensightQueue::InvalidateSession()
{
	SessionToken.Reset();
	SessionExpiresAtSeconds = 0.0;
}

bool FRavensightQueue::IsSessionValid(double NowSeconds) const
{
	return !SessionToken.IsEmpty()
		&& NowSeconds < (SessionExpiresAtSeconds - RavensightLimits::SessionExpirySkewSeconds);
}

// --- Flush lifecycle --------------------------------------------------------

FRavensightFlushPlan FRavensightQueue::BeginFlush(double NowSeconds, bool bForce)
{
	FRavensightFlushPlan Plan;

	if (!IsActive())
	{
		Plan.Decision = ERavensightFlushDecision::Disabled;
		return Plan;
	}

	if (bFlushInProgress)
	{
		Plan.Decision = ERavensightFlushDecision::AlreadyFlushing;
		return Plan;
	}

	if (Events.Num() == 0)
	{
		Plan.Decision = ERavensightFlushDecision::Empty;
		return Plan;
	}

	if (!bForce && NowSeconds < NextAttemptAtSeconds)
	{
		Plan.Decision = ERavensightFlushDecision::BackingOff;
		return Plan;
	}

	if (!IsSessionValid(NowSeconds))
	{
		// Reported rather than started, so the caller can open a session and
		// come back. The flush marker is deliberately not set here.
		Plan.Decision = ERavensightFlushDecision::NeedsSession;
		return Plan;
	}

	const int32 BatchSize = FMath::Min(RavensightLimits::MaxBatchSize, Events.Num());
	Plan.Batch.Reserve(BatchSize);
	for (int32 Index = 0; Index < BatchSize; ++Index)
	{
		Plan.Batch.Add(Events[Index]);
	}

	Plan.Decision = ERavensightFlushDecision::Send;
	bFlushInProgress = true;
	return Plan;
}

void FRavensightQueue::EndFlush()
{
	bFlushInProgress = false;
}

// --- Responses --------------------------------------------------------------

void FRavensightQueue::OnBatchAccepted(int32 BatchSize)
{
	const int32 Accepted = FMath::Clamp(BatchSize, 0, Events.Num());
	if (Accepted > 0)
	{
		Events.RemoveAt(0, Accepted);
		SentCount += Accepted;
		++BatchCount;
	}

	ResetBackoff();
}

void FRavensightQueue::OnUnauthorized()
{
	// The batch stays queued. Whatever was in flight is re sent verbatim once
	// a new session token arrives.
	InvalidateSession();
}

void FRavensightQueue::OnRateLimited(double NowSeconds, double RetryAfterSeconds)
{
	if (RetryAfterSeconds > 0.0)
	{
		NextAttemptAtSeconds = NowSeconds + RetryAfterSeconds;

		// The server told us how long to wait, so the local backoff ladder is
		// not advanced. Its next step still starts from wherever it was.
		return;
	}

	ScheduleBackoff(NowSeconds);
}

void FRavensightQueue::OnTransportFailure(double NowSeconds)
{
	ScheduleBackoff(NowSeconds);
}

// --- Backoff ----------------------------------------------------------------

void FRavensightQueue::ResetBackoff()
{
	BackoffSeconds = RavensightLimits::DefaultRetrySeconds;
	NextAttemptAtSeconds = 0.0;
}

void FRavensightQueue::ScheduleBackoff(double NowSeconds)
{
	NextAttemptAtSeconds = NowSeconds + BackoffSeconds;
	BackoffSeconds = FMath::Min(BackoffSeconds * 2.0, RavensightLimits::MaxBackoffSeconds);
}

double FRavensightQueue::SecondsUntilNextAttempt(double NowSeconds) const
{
	return FMath::Max(0.0, NextAttemptAtSeconds - NowSeconds);
}

double FRavensightQueue::GetRetryTimerSeconds(double NowSeconds) const
{
	return FMath::Max(RavensightLimits::MinRetrySeconds, SecondsUntilNextAttempt(NowSeconds));
}

// --- Stats ------------------------------------------------------------------

FRavensightQueueStats FRavensightQueue::GetStats() const
{
	FRavensightQueueStats Stats;
	Stats.Queued = Events.Num();
	Stats.Sent = SentCount;
	Stats.Dropped = DroppedCount;
	Stats.Batches = BatchCount;
	return Stats;
}
