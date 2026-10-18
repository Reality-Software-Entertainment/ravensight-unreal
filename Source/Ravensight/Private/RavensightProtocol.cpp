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

FString RavensightProtocol::ValidDistribution(const FString& InLabel)
{
	const FString Label = InLabel.TrimStartAndEnd();
	if (Label.IsEmpty() || Label.Len() > MaxDistributionLength)
	{
		return FString();
	}

	for (int32 Index = 0; Index < Label.Len(); ++Index)
	{
		const uint32 Code = static_cast<uint32>(Label[Index]);
		if (Code < 0x20 || (Code >= 0x7F && Code <= 0x9F))
		{
			return FString();
		}
	}

	return Label;
}

namespace
{
	/** Lower case hex of a non negative value, without a prefix. */
	FString Hex(int64 Value)
	{
		static const TCHAR Digits[] = TEXT("0123456789abcdef");
		uint64 Remaining = Value < 0 ? 0 : static_cast<uint64>(Value);
		FString Reversed;
		do
		{
			Reversed.AppendChar(Digits[Remaining % 16]);
			Remaining /= 16;
		} while (Remaining > 0);

		FString Out;
		for (int32 Index = Reversed.Len() - 1; Index >= 0; --Index)
		{
			Out.AppendChar(Reversed[Index]);
		}
		return Out;
	}

	/** Decimal of a non negative value. */
	FString Decimal(int64 Value)
	{
		uint64 Remaining = Value < 0 ? 0 : static_cast<uint64>(Value);
		FString Reversed;
		do
		{
			Reversed.AppendChar(static_cast<TCHAR>('0' + static_cast<int32>(Remaining % 10)));
			Remaining /= 10;
		} while (Remaining > 0);

		FString Out;
		for (int32 Index = Reversed.Len() - 1; Index >= 0; --Index)
		{
			Out.AppendChar(Reversed[Index]);
		}
		return Out;
	}

	bool IsIdChar(TCHAR Char)
	{
		return (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') || (Char >= '0' && Char <= '9') || Char == '_' || Char == '-';
	}
}

FString RavensightProtocol::EventIdPrefix(const FString& DeviceId)
{
	FString Out;
	const int32 PrefixLength = FMath::Min(DeviceId.Len(), 16);
	for (int32 Index = 0; Index < PrefixLength; ++Index)
	{
		const TCHAR Char = DeviceId[Index];
		Out.AppendChar(IsIdChar(Char) ? Char : TCHAR('_'));
	}
	return Out;
}

FString RavensightProtocol::EventId(const FString& DeviceId, int64 StartUnixSeconds, const FString& Nonce, int64 Counter)
{
	FString Out = EventIdPrefix(DeviceId);
	Out += TEXT("-");
	Out += Hex(StartUnixSeconds);
	Out += TEXT("-");
	for (int32 Index = 0; Index < Nonce.Len(); ++Index)
	{
		const TCHAR Char = Nonce[Index];
		Out.AppendChar(IsIdChar(Char) ? Char : TCHAR('_'));
	}
	Out += TEXT("-");
	Out += Decimal(Counter);
	return Out;
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

	bool RavensightConfigProtocol::Name(const FString& Name, int32 Limit, bool bVariant)
	{
		if (Name.IsEmpty() || Name.Len() > Limit || Name[0] < TCHAR('a') || Name[0] > TCHAR('z')) return false;
		for (int32 I = 0; I < Name.Len(); ++I)
		{
            const TCHAR C = Name[I];
			if (!(C >= TCHAR('a') && C <= TCHAR('z')) && !(C >= TCHAR('0') && C <= TCHAR('9')) && C != TCHAR('_') && !(bVariant && C == TCHAR('-'))) return false;
		}
		return true;
	}

bool RavensightConfigProtocol::ETag(const FString& Value)
{
 if (Value.Len() != 66 || Value[0] != TCHAR('"') || Value[65] != TCHAR('"')) return false;
 for (int32 I = 1; I < 65; ++I) if (!(Value[I] >= TCHAR('0') && Value[I] <= TCHAR('9')) && !(Value[I] >= TCHAR('a') && Value[I] <= TCHAR('f'))) return false;
 return true;
}
bool RavensightConfigProtocol::BoundedDocument(const FString& Body)
{
 if (Body.IsEmpty() || Body.Len() > 450000) return false;
 bool bString = false, bEscape = false; int32 Depth = 0;
 for (int32 I = 0; I < Body.Len(); ++I)
 {
  const TCHAR C = Body[I];
  if (bString) { if (bEscape) bEscape = false; else if (C == TCHAR('\\')) bEscape = true; else if (C == TCHAR('"')) bString = false; }
  else if (C == TCHAR('"')) bString = true;
  else if (C == TCHAR('{') || C == TCHAR('[')) { if (++Depth > 24) return false; }
  else if (C == TCHAR('}') || C == TCHAR(']')) { if (--Depth < 0) return false; }
 }
 return !bString && Depth == 0;
}
