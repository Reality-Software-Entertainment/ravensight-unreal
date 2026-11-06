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
		// Queued errors go before any gameplay event, never from inside a
		// batch in flight. Then the oldest goes, so the newest events always
		// survive: a player who has been offline for an hour is better served
		// by the last minute of their session than by the first.
		const int32 Victim = FirstQueuedError();
		Events.RemoveAt(Victim == INDEX_NONE ? 0 : Victim);
		if (Victim == INDEX_NONE && InFlightCount > 0)
		{
			--InFlightCount;
		}
		++Dropped;
	}

	DroppedCount += Dropped;
	return Dropped;
}

int32 FRavensightQueue::FirstQueuedError() const
{
	for (int32 Index = FMath::Min(InFlightCount, Events.Num()); Index < Events.Num(); ++Index)
	{
		if (Events[Index].bError)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 FRavensightQueue::Enqueue(const FRavensightEvent& Event)
{
	if (!IsActive())
	{
		return 0;
	}

	if (Event.bError && Events.Num() >= MaxQueueSize && FirstQueuedError() == INDEX_NONE)
	{
		// Full of gameplay events: the incoming error is the one that goes.
		++DroppedCount;
		return 1;
	}

	const int32 Dropped = TrimToCap(1);
	const int32 At = Event.bError ? INDEX_NONE : FirstQueuedError();
	if (At == INDEX_NONE)
	{
		Events.Add(Event);
	}
	else
	{
		Events.Insert(Event, At);
	}
	return Dropped;
}

int32 FRavensightQueue::Restore(const TArray<FRavensightEvent>& Restored)
{
	if (!IsActive() || bFlushInProgress || Restored.Num() == 0)
	{
		return 0;
	}
	TArray<FRavensightEvent> Merged;
	Merged.Reserve(Restored.Num() + Events.Num());
	for (const FRavensightEvent& Event : Restored)
	{
		Merged.Add(Event);
	}
	for (const FRavensightEvent& Event : Events)
	{
		Merged.Add(Event);
	}
	Events = Merged;
	TrimToCap(0);
	return Restored.Num();
}

void FRavensightQueue::StampOrigins(const FString& InOriginJson, const FString& InOriginSession)
{
	if (InOriginJson.IsEmpty())
	{
		return;
	}
	for (FRavensightEvent& Event : Events)
	{
		if (Event.OriginJson.IsEmpty())
		{
			Event.OriginJson = InOriginJson;
			Event.OriginSession = InOriginSession;
		}
	}
}

int32 FRavensightQueue::EnqueueAtFront(const FRavensightEvent& Event)
{
	if (!IsActive())
	{
		return 0;
	}

	// A batch in flight is a copy of the leading events and leaves the queue
	// by count on a 202, so nothing may be inserted ahead of it.
	if (bFlushInProgress)
	{
		return Enqueue(Event);
	}

	const int32 Dropped = TrimToCap(1);
	Events.Insert(Event, 0);
	return Dropped;
}

void FRavensightQueue::ReplaceDataAt(int32 Index, const FString& InDataJson)
{
	if (bFlushInProgress || Index < 0 || Index >= Events.Num())
	{
		return;
	}
	Events[Index].DataJson = InDataJson;
}

void FRavensightQueue::Clear()
{
	DroppedCount += Events.Num();
	Events.Reset();
}

int32 FRavensightQueue::DropAt(int32 Index, int32 Count)
{
	if (Index < 0 || Index >= Events.Num())
	{
		return 0;
	}
	const int32 ToDrop = FMath::Clamp(Count, 0, Events.Num() - Index);
	if (ToDrop > 0)
	{
		Events.RemoveAt(Index, ToDrop);
		DroppedCount += ToDrop;
	}
	return ToDrop;
}

int32 FRavensightQueue::DropHead(int32 Count)
{
	const int32 ToDrop = FMath::Clamp(Count, 0, Events.Num());
	if (ToDrop > 0)
	{
		Events.RemoveAt(0, ToDrop);
		DroppedCount += ToDrop;
	}
	return ToDrop;
}

// --- Enablement -------------------------------------------------------------

void FRavensightQueue::SetEnabled(bool bInEnabled)
{
	if (bInEnabled == bEnabledSetting)
	{
		return;
	}

	bEnabledSetting = bInEnabled;
	if (!bInEnabled)
	{
		Clear();
	}
}

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

FRavensightFlushPlan FRavensightQueue::BeginFlush(double NowSeconds, bool bForce, int32 MaxCount)
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

	if (bSendBlocked)
	{
		Plan.Decision = ERavensightFlushDecision::Refused;
		return Plan;
	}

	if ((!bForce || bRetryAfterActive) && NowSeconds < NextAttemptAtSeconds)
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

	const int32 Limit = FMath::Clamp(MaxCount, 1, RavensightLimits::MaxBatchSize);
	const int32 BatchSize = FMath::Min(Limit, Events.Num());
	Plan.Batch.Reserve(BatchSize);
	for (int32 Index = 0; Index < BatchSize; ++Index)
	{
		Plan.Batch.Add(Events[Index]);
	}

	Plan.Decision = ERavensightFlushDecision::Send;
	bFlushInProgress = true;
	InFlightCount = BatchSize;
	return Plan;
}

void FRavensightQueue::EndFlush()
{
	bFlushInProgress = false;
	InFlightCount = 0;
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
		bRetryAfterActive = true;

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
	bRetryAfterActive = false;
}

void FRavensightQueue::ScheduleBackoff(double NowSeconds)
{
	const double Random = RandomSource ? RandomSource() : static_cast<double>(FMath::FRand());
	bRetryAfterActive = false;
	NextAttemptAtSeconds = NowSeconds + JitteredWaitSeconds(BackoffSeconds, Random);
	BackoffSeconds = FMath::Min(BackoffSeconds * 2.0, RavensightLimits::MaxBackoffSeconds);
}

double FRavensightQueue::JitteredWaitSeconds(double InBackoffSeconds, double Random)
{
	// NaN fails both comparisons and lands on 0.
	const double Clamped = Random >= 1.0 ? 1.0 : (Random > 0.0 ? Random : 0.0);
	const double Factor = RavensightLimits::JitterFloor + (1.0 - RavensightLimits::JitterFloor) * Clamped;
	return FMath::Clamp(InBackoffSeconds * Factor, RavensightLimits::MinRetrySeconds, RavensightLimits::MaxBackoffSeconds);
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
