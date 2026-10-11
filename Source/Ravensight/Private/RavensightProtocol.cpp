// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightProtocol.h"

namespace
{
	bool IsCallerReported(ERavensightEndpoint Endpoint)
	{
		// Feedback and suggestions are one shot calls whose result goes back to
		// the game. They never schedule a retry of their own.
		return Endpoint == ERavensightEndpoint::Feedback || Endpoint == ERavensightEndpoint::Suggestions;
	}

	FRavensightResponseVerdict MakeVerdict(ERavensightResponseAction Action, const TCHAR* Reason = TEXT(""))
	{
		FRavensightResponseVerdict Out;
		Out.Action = Action;
		Out.Reason = Reason;
		return Out;
	}
}

FRavensightResponseVerdict RavensightProtocol::ClassifyResponse(const FRavensightResponseInput& Input)
{
	const int32 Status = Input.Status;
	const ERavensightEndpoint Endpoint = Input.Endpoint;

	if (Status >= 200 && Status < 300)
	{
		return MakeVerdict(ERavensightResponseAction::Accept);
	}

	// The three 403 bodies that are not about the key come first: they are
	// permanent for the run whatever endpoint answered.
	if (Status == 403)
	{
		if (Input.Error == TEXT("tracking_disabled"))
		{
			return MakeVerdict(ERavensightResponseAction::KillSwitch, RavensightReason::TrackingDisabled);
		}
		if (Input.Error == TEXT("playtest_job_closed"))
		{
			return MakeVerdict(ERavensightResponseAction::PlaytestStop, RavensightReason::PlaytestJobClosed);
		}
		if (Input.Error == TEXT("invalid_playtest_token"))
		{
			return MakeVerdict(ERavensightResponseAction::PlaytestStop, RavensightReason::SessionFailed);
		}
		return MakeVerdict(ERavensightResponseAction::FatalKey, RavensightReason::InvalidKey);
	}

	if (Status == 401)
	{
		// The batch and feedback endpoints authenticate with the session
		// token, so a 401 there means the session expired, not the key.
		if (Endpoint == ERavensightEndpoint::Batch)
		{
			if (Input.ConsecutiveBatchUnauthorized <= RavensightProtocol::MaxBatchReauthInARow)
			{
				return MakeVerdict(ERavensightResponseAction::RefreshSession);
			}
			return MakeVerdict(ERavensightResponseAction::BackOff);
		}
		if (Endpoint == ERavensightEndpoint::Feedback)
		{
			return MakeVerdict(ERavensightResponseAction::RefreshSession);
		}
		return MakeVerdict(ERavensightResponseAction::FatalKey, RavensightReason::InvalidKey);
	}

	if (IsCallerReported(Endpoint))
	{
		return MakeVerdict(ERavensightResponseAction::Fail);
	}

	if (Status == 0)
	{
		return MakeVerdict(ERavensightResponseAction::BackOff, RavensightReason::Offline);
	}

	if (Status == 429)
	{
		return MakeVerdict(ERavensightResponseAction::WaitRetryAfter, RavensightReason::RateLimited);
	}

	if (Status == 503 && Input.bHasRetryAfter)
	{
		return MakeVerdict(
			ERavensightResponseAction::WaitRetryAfter,
			Endpoint == ERavensightEndpoint::Session ? RavensightReason::SessionFailed : TEXT(""));
	}

	if (Status == 400 && Endpoint == ERavensightEndpoint::Batch)
	{
		return MakeVerdict(Input.BatchSize > 1 ? ERavensightResponseAction::HalveBatch : ERavensightResponseAction::DropHead);
	}

	return MakeVerdict(
		ERavensightResponseAction::BackOff,
		Endpoint == ERavensightEndpoint::Session ? RavensightReason::SessionFailed : TEXT(""));
}

FString RavensightProtocol::NormalizeApiUrl(const FString& InUrl)
{
	FString Raw = InUrl.TrimStartAndEnd();
	while (Raw.EndsWith(TEXT("/")))
	{
		Raw.LeftChopInline(1);
	}

	if (Raw.EndsWith(TEXT("/api/v1")))
	{
		Raw.LeftChopInline(7);
	}

	if (Raw.IsEmpty())
	{
		Raw = TEXT("https://api.ravensight.io");
	}

	return Raw + TEXT("/api/v1");
}

const TArray<FString>& RavensightProtocol::FeedbackCategories()
{
	static const TArray<FString> Categories = {
		TEXT("bug"),
		TEXT("suggestion"),
		TEXT("complaint"),
		TEXT("praise"),
		TEXT("playtest"),
		TEXT("other"),
	};
	return Categories;
}

FString RavensightProtocol::ValidateFeedback(const FString& Message, const FString& Category, int32 Rating)
{
	if (Message.TrimStartAndEnd().IsEmpty())
	{
		return TEXT("empty_message");
	}

	// Case sensitive, like the server: FString's own == ignores case.
	const bool bKnownCategory = FeedbackCategories().ContainsByPredicate([&Category](const FString& Known)
	{
		return Known.Equals(Category, ESearchCase::CaseSensitive);
	});

	if (!Category.IsEmpty() && !bKnownCategory)
	{
		return TEXT("invalid_category");
	}

	if (Rating < 0 || Rating > 5)
	{
		return TEXT("invalid_rating");
	}

	return FString();
}

bool RavensightProtocol::ShouldEndSessionAfterBackground(double NowSeconds, double BackgroundedAtSeconds, double SessionTimeoutSeconds)
{
	if (BackgroundedAtSeconds <= 0.0)
	{
		return false;
	}

	return (NowSeconds - BackgroundedAtSeconds) >= SessionTimeoutSeconds;
}

bool RavensightProtocol::ShouldReportMarkerExit(double NowSeconds, const FString& MarkerToken, double MarkerExpiresAtSeconds, bool bOptedOut)
{
	return !bOptedOut && !MarkerToken.IsEmpty() && NowSeconds < MarkerExpiresAtSeconds;
}

RavensightProtocol::FPlaytestResolution RavensightProtocol::ResolvePlaytest(const FPlaytestSources& Sources)
{
	FPlaytestResolution Out;

	// First source with a token wins, and its run id comes from the same
	// source so a stale variable cannot pair with a fresh command line.
	if (!Sources.ExplicitToken.IsEmpty())
	{
		Out.Token = Sources.ExplicitToken;
		Out.RunId = Sources.ExplicitRunId;
	}
	else if (!Sources.EnvToken.IsEmpty())
	{
		Out.Token = Sources.EnvToken;
		Out.RunId = Sources.EnvRunId;
	}
	else if (!Sources.CommandLineToken.IsEmpty())
	{
		Out.Token = Sources.CommandLineToken;
		Out.RunId = Sources.CommandLineRunId;
	}

	Out.Token.TrimStartAndEndInline();
	Out.RunId.TrimStartAndEndInline();
	return Out;
}

FString RavensightProtocol::NextStatusReason(const FString& Current, const FRavensightResponseVerdict& Verdict)
{
	if (Verdict.Action == ERavensightResponseAction::Accept)
	{
		return FString();
	}

	return Verdict.Reason.IsEmpty() ? Current : Verdict.Reason;
}

FRavensightGate RavensightProtocol::DecideGate(const FRavensightGateInput& Input)
{
	FRavensightGate Gate;

	if (Input.bPlaytestMissingRunId)
	{
		Gate.Reason = RavensightReason::SessionFailed;
		return Gate;
	}

	if (Input.bStoppedForRun)
	{
		return Gate;
	}

	const bool bOptedIn = Input.bPlayerOptedIn || Input.bPlaytest;
	if (!Input.bEnabled || !bOptedIn)
	{
		Gate.Reason = RavensightReason::Disabled;
		return Gate;
	}

	// Events may be held before Init and while the key is refused.
	Gate.bAllowEnqueue = true;

	if (!Input.bInitialized)
	{
		return Gate;
	}

	if (Input.bKeyRefused)
	{
		Gate.Reason = RavensightReason::InvalidKey;
		Gate.bAllowSession = Input.bExplicitAttempt;
		Gate.bAllowBatch = Input.bExplicitAttempt;
		return Gate;
	}

	Gate.bAllowSettings = true;
	Gate.bAllowSession = true;
	Gate.bAllowBatch = true;
	Gate.bAllowSuggestions = true;
	Gate.bAllowFeedback = true;
	return Gate;
}

// --- Lifecycle guards -------------------------------------------------------

bool FRavensightLifecycleGuards::TryClaimStarted()
{
	if (bStarted)
	{
		return false;
	}
	bStarted = true;
	return true;
}

bool FRavensightLifecycleGuards::TryClaimExited()
{
	if (bExited)
	{
		return false;
	}
	bExited = true;
	return true;
}

void FRavensightLifecycleGuards::ResetForNewSession()
{
	bStarted = false;
}
