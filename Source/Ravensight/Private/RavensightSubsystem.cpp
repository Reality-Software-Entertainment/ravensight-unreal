// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RavensightSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogRavensight);

namespace
{
	/** Where the generated device id is persisted between runs. */
	const TCHAR* const DeviceIdSection = TEXT("Ravensight");
	const TCHAR* const DeviceIdKey = TEXT("DeviceId");

	/** Session lifetime assumed when the server sends neither expiresAt nor expiresIn. */
	constexpr double DefaultSessionSeconds = 86400.0;

	FString WriteJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;

		// Non const on purpose: FJsonSerializer::Serialize takes the writer by
		// mutable reference.
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);

		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}
}

// --- Lifecycle --------------------------------------------------------------

void URavensightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const URavensightSettings* Settings = URavensightSettings::Get();

	FlushIntervalSeconds = Settings->FlushIntervalSeconds;
	RequestTimeoutSeconds = Settings->RequestTimeoutSeconds;
	bTrackLifecycleEvents = Settings->bTrackLifecycleEvents;
	bVerboseLogging = Settings->bVerboseLogging;

	Queue.SetMaxQueueSize(Settings->MaxQueueSize);

	BindApplicationDelegates();

	if (!Settings->bAutoInitialize)
	{
		Log(TEXT("auto initialize is off, waiting for an explicit Init call"));
		return;
	}

	if (Settings->IngestKey.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("No ingest key set. Fill in Project Settings > Plugins > Ravensight, or call Init from code. Nothing will be sent until then."));
		return;
	}

	Init(Settings->IngestKey, Settings->ApiUrl, Settings->GameVersion);
}

void URavensightSubsystem::Deinitialize()
{
	bShuttingDown = true;

	if (bInitialized && bTrackLifecycleEvents && Queue.IsActive())
	{
		TrackEvent(TEXT("game_exited"), TMap<FString, FString>());
	}

	// Best effort. The engine does not pump HTTP after this point on every
	// platform, so the last batch can be lost. Flush at points you control if
	// the final events matter to you.
	FlushInternal(true);

	StopTimers();
	UnbindApplicationDelegates();

	Super::Deinitialize();
}

URavensightSubsystem* URavensightSubsystem::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject || !GEngine)
	{
		return nullptr;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	if (!World)
	{
		return nullptr;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<URavensightSubsystem>() : nullptr;
}

// --- Setup ------------------------------------------------------------------

FString URavensightSubsystem::NormalizeApiUrl(const FString& InUrl)
{
	FString Raw = InUrl.TrimStartAndEnd();
	while (Raw.EndsWith(TEXT("/")))
	{
		Raw.LeftChopInline(1);
	}

	if (Raw.IsEmpty())
	{
		return TEXT("https://api.ravensight.io/api/v1");
	}

	if (Raw.EndsWith(TEXT("/api/v1")))
	{
		return Raw;
	}

	return Raw + TEXT("/api/v1");
}

void URavensightSubsystem::Init(const FString& InIngestKey, const FString& InApiUrl, const FString& InGameVersion)
{
	if (bInitialized)
	{
		Log(TEXT("Init called again, ignoring"));
		return;
	}

	IngestKey = InIngestKey.TrimStartAndEnd();
	if (IngestKey.IsEmpty())
	{
		UE_LOG(LogRavensight, Warning, TEXT("Init called with an empty ingest key. Nothing will be sent."));
		return;
	}

	const URavensightSettings* Settings = URavensightSettings::Get();

	ApiUrl = NormalizeApiUrl(InApiUrl.IsEmpty() ? Settings->ApiUrl : InApiUrl);
	GameVersion = InGameVersion.IsEmpty() ? ResolveGameVersion() : InGameVersion;
	Platform = UGameplayStatics::GetPlatformName();
	DeviceId = ResolveDeviceId();

	bInitialized = true;

	Log(FString::Printf(TEXT("initialized against %s as %s on %s"), *ApiUrl, *GameVersion, *Platform));

	StartFlushTimer();
	CheckServerSettings();
}

void URavensightSubsystem::SetTrackingEnabled(bool bEnabled)
{
	if (bEnabled == Queue.IsLocallyEnabled())
	{
		return;
	}

	Queue.SetLocallyEnabled(bEnabled);
	Log(bEnabled ? TEXT("tracking opted in") : TEXT("tracking opted out, queue discarded"));

	if (bEnabled && bInitialized && !bSettingsChecked)
	{
		CheckServerSettings();
	}
}

// --- Tracking ---------------------------------------------------------------

void URavensightSubsystem::TrackEvent(const FString& EventName, const TMap<FString, FString>& Data)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Data)
	{
		Payload->SetStringField(Pair.Key, Pair.Value);
	}

	TrackEventJson(EventName, WriteJson(Payload));
}

void URavensightSubsystem::TrackEventJson(const FString& EventName, const FString& JsonData)
{
	if (EventName.IsEmpty())
	{
		UE_LOG(LogRavensight, Warning, TEXT("TrackEvent called with an empty event name, ignoring"));
		return;
	}

	if (!Queue.IsActive())
	{
		return;
	}

	FString DataJson = JsonData.TrimStartAndEnd();
	if (DataJson.IsEmpty())
	{
		DataJson = TEXT("{}");
	}
	else if (!ParseJsonObject(DataJson).IsValid())
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("Event '%s' carried a payload that is not a JSON object, sending it with no data"), *EventName);
		DataJson = TEXT("{}");
	}

	const int64 Timestamp = FDateTime::UtcNow().ToUnixTimestamp();
	const int32 Dropped = Queue.Enqueue(FRavensightEvent(EventName, DataJson, Timestamp));
	if (Dropped > 0)
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("Queue is full at %d events, dropped %d oldest to make room"), Queue.GetMaxQueueSize(), Dropped);
	}

	if (!bInitialized || !bSettingsChecked)
	{
		// Held until the kill switch answers. Nothing is lost: the queue keeps
		// them and the first flush after boot sends them.
		return;
	}

	FlushInternal(false);
}

void URavensightSubsystem::Flush()
{
	FlushInternal(true);
}

void URavensightSubsystem::FlushInternal(bool bForce)
{
	if (!bInitialized)
	{
		return;
	}

	const FRavensightFlushPlan Plan = Queue.BeginFlush(NowSeconds(), bForce);

	switch (Plan.Decision)
	{
	case ERavensightFlushDecision::Send:
		SendBatch(Plan.Batch);
		break;

	case ERavensightFlushDecision::NeedsSession:
		EnsureSession();
		break;

	default:
		// Empty, AlreadyFlushing, BackingOff and Disabled all mean there is
		// nothing useful to do right now. The flush timer comes back around.
		break;
	}
}

void URavensightSubsystem::SendBatch(const TArray<FRavensightEvent>& Batch)
{
	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(Batch.Num());

	for (const FRavensightEvent& Event : Batch)
	{
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("event"), Event.EventName);

		TSharedPtr<FJsonObject> Data = ParseJsonObject(Event.DataJson);
		if (!Data.IsValid())
		{
			Data = MakeShared<FJsonObject>();
		}
		Entry->SetObjectField(TEXT("data"), Data);
		Entry->SetNumberField(TEXT("timestamp"), static_cast<double>(Event.TimestampUnix));

		EventValues.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("events"), EventValues);

	const FHttpRequestRef Request = MakeRequest(TEXT("POST"), TEXT("/track/batch"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-Session-Token"), Queue.GetSessionToken());
	Request->SetContentAsString(WriteJson(Body));
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleBatchResponse, Batch.Num());

	if (!Request->ProcessRequest())
	{
		Queue.EndFlush();
		Queue.OnTransportFailure(NowSeconds());
		OnFlushFailed.Broadcast(TEXT("request_not_started"));
		ScheduleRetry();
	}
}

void URavensightSubsystem::HandleBatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 BatchSize)
{
	Queue.EndFlush();

	const double Now = NowSeconds();

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		Log(TEXT("batch flush could not reach the server, will retry"));
		Queue.OnTransportFailure(Now);
		OnFlushFailed.Broadcast(TEXT("unreachable"));
		ScheduleRetry();
		return;
	}

	const int32 Code = Response->GetResponseCode();

	if (Code == 202)
	{
		Queue.OnBatchAccepted(BatchSize);
		Log(FString::Printf(TEXT("sent %d events, %d still queued"), BatchSize, Queue.Num()));
		OnEventsFlushed.Broadcast(BatchSize);

		if (!Queue.IsEmpty() && !bShuttingDown)
		{
			// Keep draining while the server is happy to take them.
			FlushInternal(false);
		}
		return;
	}

	if (Code == 401)
	{
		// Expired or revoked. The batch stays queued and goes back out as soon
		// as a fresh token is issued.
		Log(TEXT("session invalid or expired, refreshing"));
		Queue.OnUnauthorized();
		EnsureSession();
		return;
	}

	const FString Body = ResponseBody(Response);

	if (Code == 429)
	{
		const double RetryAfter = ParseRetryAfterSeconds(Response);
		Queue.OnRateLimited(Now, RetryAfter);

		const FString Reason = ExtractError(Body, TEXT("rate_limited"));
		UE_LOG(LogRavensight, Warning, TEXT("track/batch rate limited (%s), retrying in %.0fs"),
			*Reason, Queue.SecondsUntilNextAttempt(Now));

		OnFlushFailed.Broadcast(Reason);
		ScheduleRetry();
		return;
	}

	const FString Reason = ExtractError(Body, FString::Printf(TEXT("http_%d"), Code));
	UE_LOG(LogRavensight, Warning, TEXT("batch flush failed (HTTP %d), will retry"), Code);
	Queue.OnTransportFailure(Now);
	OnFlushFailed.Broadcast(Reason);
	ScheduleRetry();
}

// --- Server kill switch -----------------------------------------------------

void URavensightSubsystem::CheckServerSettings()
{
	const FHttpRequestRef Request = MakeRequest(TEXT("GET"), TEXT("/settings"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSettingsResponse);

	if (!Request->ProcessRequest())
	{
		UE_LOG(LogRavensight, Warning, TEXT("settings check could not start, assuming tracking is enabled"));
		AfterSettingsChecked();
	}
}

void URavensightSubsystem::HandleSettingsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bool bServerEnabled = true;

	if (bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() == 200)
	{
		const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(ResponseBody(Response));
		if (Parsed.IsValid())
		{
			bool bParsed = false;
			if (Parsed->TryGetBoolField(TEXT("trackingEnabled"), bParsed))
			{
				bServerEnabled = bParsed;
			}
		}
	}
	else
	{
		// An unreachable settings endpoint must not silently stop analytics.
		// Same call as the Godot SDK makes: assume on.
		UE_LOG(LogRavensight, Warning, TEXT("could not read settings, assuming tracking is enabled"));
	}

	Queue.SetServerTrackingEnabled(bServerEnabled);
	AfterSettingsChecked();
}

void URavensightSubsystem::AfterSettingsChecked()
{
	bSettingsChecked = true;

	if (!Queue.IsServerTrackingEnabled())
	{
		UE_LOG(LogRavensight, Log, TEXT("Tracking disabled by the server kill switch. Nothing will be sent this run."));
		StopTimers();
		OnTrackingDisabled.Broadcast(TEXT("server"));
		return;
	}

	if (bTrackLifecycleEvents)
	{
		// Enqueuing this also gets the session started, exactly as the Godot
		// SDK does on boot.
		TrackEvent(TEXT("game_started"), TMap<FString, FString>());
	}
	else
	{
		FlushInternal(false);
	}
}

// --- Session ----------------------------------------------------------------

void URavensightSubsystem::EnsureSession()
{
	if (!bInitialized || bSessionRequestPending || !Queue.IsActive())
	{
		return;
	}

	if (Queue.IsSessionValid(NowSeconds()))
	{
		return;
	}

	bSessionRequestPending = true;

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("deviceId"), DeviceId);
	Body->SetStringField(TEXT("gameVersion"), GameVersion);
	Body->SetStringField(TEXT("platform"), Platform);

	const FHttpRequestRef Request = MakeRequest(TEXT("POST"), TEXT("/session"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->SetContentAsString(WriteJson(Body));
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSessionResponse);

	if (!Request->ProcessRequest())
	{
		bSessionRequestPending = false;
		UE_LOG(LogRavensight, Error, TEXT("session request could not start"));
		Queue.OnTransportFailure(NowSeconds());
		OnSessionFailed.Broadcast(TEXT("request_not_started"));
		ScheduleRetry();
	}
}

void URavensightSubsystem::HandleSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	bSessionRequestPending = false;

	const double Now = NowSeconds();

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		Queue.OnTransportFailure(Now);
		OnSessionFailed.Broadcast(TEXT("unreachable"));
		ScheduleRetry();
		return;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Body = ResponseBody(Response);

	if (Code == 201)
	{
		const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(Body);
		FString Token;
		if (Parsed.IsValid() && Parsed->TryGetStringField(TEXT("token"), Token) && !Token.IsEmpty())
		{
			double ExpiresAt = 0.0;
			if (!Parsed->TryGetNumberField(TEXT("expiresAt"), ExpiresAt) || ExpiresAt <= 0.0)
			{
				double ExpiresIn = 0.0;
				if (!Parsed->TryGetNumberField(TEXT("expiresIn"), ExpiresIn) || ExpiresIn <= 0.0)
				{
					ExpiresIn = DefaultSessionSeconds;
				}
				ExpiresAt = Now + ExpiresIn;
			}

			Queue.SetSession(Token, ExpiresAt);
			Queue.ResetBackoff();

			Log(TEXT("session ready"));
			OnSessionReady.Broadcast();

			FlushInternal(false);
			return;
		}

		UE_LOG(LogRavensight, Error, TEXT("session response was missing a token"));
		Queue.OnTransportFailure(Now);
		OnSessionFailed.Broadcast(TEXT("malformed_response"));
		ScheduleRetry();
		return;
	}

	if (Code == 429)
	{
		const double RetryAfter = ParseRetryAfterSeconds(Response);
		Queue.OnRateLimited(Now, RetryAfter);

		const FString Reason = ExtractError(Body, TEXT("rate_limited"));
		UE_LOG(LogRavensight, Warning, TEXT("session creation rate limited (%s), retrying in %.0fs"),
			*Reason, Queue.SecondsUntilNextAttempt(Now));

		OnSessionFailed.Broadcast(Reason);
		ScheduleRetry();
		return;
	}

	const FString Reason = ExtractError(Body, FString::Printf(TEXT("http_%d"), Code));
	UE_LOG(LogRavensight, Error, TEXT("session creation failed (HTTP %d)"), Code);
	Queue.OnTransportFailure(Now);
	OnSessionFailed.Broadcast(Reason);
	ScheduleRetry();
}

// --- Feedback ---------------------------------------------------------------

void URavensightSubsystem::SubmitFeedback(const FString& Message, const FString& FeedbackCategory, int32 Rating)
{
	if (Message.TrimStartAndEnd().IsEmpty())
	{
		OnFeedbackResult.Broadcast(false, TEXT("empty_message"));
		return;
	}

	if (!bInitialized || !Queue.IsActive())
	{
		OnFeedbackResult.Broadcast(false, TEXT("tracking_disabled"));
		return;
	}

	if (!Queue.IsSessionValid(NowSeconds()))
	{
		// Feedback is not queued: unlike an event it is tied to a moment in
		// the player's session, and a silent delayed send would be worse than
		// telling the caller to try again.
		OnFeedbackResult.Broadcast(false, TEXT("no_session"));
		EnsureSession();
		return;
	}

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("message"), Message);
	if (!FeedbackCategory.IsEmpty())
	{
		Body->SetStringField(TEXT("category"), FeedbackCategory);
	}
	if (Rating > 0)
	{
		Body->SetNumberField(TEXT("rating"), Rating);
	}

	const FHttpRequestRef Request = MakeRequest(TEXT("POST"), TEXT("/feedback"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-Session-Token"), Queue.GetSessionToken());
	Request->SetContentAsString(WriteJson(Body));
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleFeedbackResponse);

	if (!Request->ProcessRequest())
	{
		OnFeedbackResult.Broadcast(false, TEXT("request_not_started"));
	}
}

void URavensightSubsystem::HandleFeedbackResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		OnFeedbackResult.Broadcast(false, TEXT("unreachable"));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code == 201)
	{
		OnFeedbackResult.Broadcast(true, FString());
		return;
	}

	if (Code == 401)
	{
		// Let the next call succeed rather than silently retrying a message
		// the player thinks they already sent.
		Queue.OnUnauthorized();
		EnsureSession();
	}

	const FString Reason = ExtractError(ResponseBody(Response), FString::Printf(TEXT("http_%d"), Code));
	UE_LOG(LogRavensight, Warning, TEXT("feedback submission failed (HTTP %d)"), Code);
	OnFeedbackResult.Broadcast(false, Reason);
}

// --- Suggestions ------------------------------------------------------------

void URavensightSubsystem::FetchSuggestions()
{
	if (!bInitialized)
	{
		OnSuggestionsReceived.Broadcast(false, TArray<FString>());
		return;
	}

	const FHttpRequestRef Request = MakeRequest(TEXT("GET"), TEXT("/agent/suggestions"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSuggestionsResponse);

	if (!Request->ProcessRequest())
	{
		OnSuggestionsReceived.Broadcast(false, TArray<FString>());
	}
}

void URavensightSubsystem::HandleSuggestionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	TArray<FString> Suggestions;

	if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() != 200)
	{
		const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
		UE_LOG(LogRavensight, Warning, TEXT("fetch suggestions failed (HTTP %d)"), Code);
		OnSuggestionsReceived.Broadcast(false, Suggestions);
		return;
	}

	const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(ResponseBody(Response));
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Parsed.IsValid() && Parsed->TryGetArrayField(TEXT("suggestions"), Array) && Array)
	{
		Suggestions.Reserve(Array->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			// Handed back as JSON strings on purpose: the server side shape is
			// still moving, and a USTRUCT here would break on the next change.
			const TSharedPtr<FJsonObject>* AsObject = nullptr;
			if (Value->TryGetObject(AsObject) && AsObject && AsObject->IsValid())
			{
				Suggestions.Add(WriteJson((*AsObject).ToSharedRef()));
			}
			else
			{
				FString AsString;
				if (Value->TryGetString(AsString))
				{
					Suggestions.Add(AsString);
				}
			}
		}
	}

	OnSuggestionsReceived.Broadcast(true, Suggestions);
}

// --- State ------------------------------------------------------------------

bool URavensightSubsystem::IsSessionReady() const
{
	return Queue.IsSessionValid(NowSeconds());
}

bool URavensightSubsystem::IsTrackingActive() const
{
	return Queue.IsActive();
}

int32 URavensightSubsystem::GetQueuedEventCount() const
{
	return Queue.Num();
}

FString URavensightSubsystem::GetDeviceId() const
{
	return DeviceId;
}

// --- Timers -----------------------------------------------------------------

void URavensightSubsystem::StartFlushTimer()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || FlushIntervalSeconds <= 0.0f)
	{
		return;
	}

	GameInstance->GetTimerManager().SetTimer(
		FlushTimerHandle, this, &URavensightSubsystem::OnFlushTimer, FlushIntervalSeconds, true);
}

void URavensightSubsystem::StopTimers()
{
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		FTimerManager& Timers = GameInstance->GetTimerManager();
		Timers.ClearTimer(FlushTimerHandle);
		Timers.ClearTimer(RetryTimerHandle);
	}
}

void URavensightSubsystem::ScheduleRetry()
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || bShuttingDown)
	{
		return;
	}

	const float Wait = static_cast<float>(Queue.GetRetryTimerSeconds(NowSeconds()));
	GameInstance->GetTimerManager().SetTimer(
		RetryTimerHandle, this, &URavensightSubsystem::OnRetryTimer, Wait, false);
}

void URavensightSubsystem::OnFlushTimer()
{
	FlushInternal(false);
}

void URavensightSubsystem::OnRetryTimer()
{
	if (!Queue.IsActive())
	{
		return;
	}

	if (!Queue.IsSessionValid(NowSeconds()))
	{
		EnsureSession();
		return;
	}

	FlushInternal(false);
}

// --- Application lifecycle --------------------------------------------------

void URavensightSubsystem::BindApplicationDelegates()
{
	DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(
		this, &URavensightSubsystem::OnApplicationWillDeactivate);

	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(
		this, &URavensightSubsystem::OnApplicationWillDeactivate);

	ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(
		this, &URavensightSubsystem::OnApplicationEnteredForeground);

	// Standing in for an actor's EndPlay: the last moment a level is still
	// around and HTTP is still being pumped.
	TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(
		this, &URavensightSubsystem::OnWorldBeginTearDown);
}

void URavensightSubsystem::UnbindApplicationDelegates()
{
	FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);

	DeactivateHandle.Reset();
	BackgroundHandle.Reset();
	ForegroundHandle.Reset();
	TearDownHandle.Reset();
}

void URavensightSubsystem::OnApplicationWillDeactivate()
{
	if (!bInitialized || !Queue.IsActive())
	{
		return;
	}

	if (bTrackLifecycleEvents)
	{
		TrackEvent(TEXT("game_paused"), TMap<FString, FString>());
	}

	// The most important flush on mobile: iOS and Android can suspend or kill
	// the process without ever returning to the foreground.
	FlushInternal(true);
}

void URavensightSubsystem::OnApplicationEnteredForeground()
{
	if (!bInitialized || !Queue.IsActive() || !bTrackLifecycleEvents)
	{
		return;
	}

	TrackEvent(TEXT("game_resumed"), TMap<FString, FString>());
}

void URavensightSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	if (!bInitialized || !World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	FlushInternal(true);
}

// --- Helpers ----------------------------------------------------------------

FHttpRequestRef URavensightSubsystem::MakeRequest(const FString& Verb, const FString& Path) const
{
	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(Verb);
	Request->SetURL(ApiUrl + Path);
	Request->SetTimeout(RequestTimeoutSeconds);
	return Request;
}

double URavensightSubsystem::NowSeconds()
{
	return static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp());
}

FString URavensightSubsystem::ResolveGameVersion()
{
	// Project Settings > Project > Description > Version, the field a team is
	// already bumping at release time.
	FString ProjectVersion;
	if (GConfig)
	{
		GConfig->GetString(
			TEXT("/Script/EngineSettings.GeneralProjectSettings"),
			TEXT("ProjectVersion"),
			ProjectVersion,
			GGameIni);
	}

	return ProjectVersion.IsEmpty() ? FString(TEXT("1.0.0")) : ProjectVersion;
}

FString URavensightSubsystem::ResolveDeviceId()
{
	// Deliberately not FPlatformMisc::GetDeviceId: it is deprecated, returns
	// nothing at all on several platforms, and on the ones where it works it
	// is closer to a hardware identifier than we want. A random GUID kept in
	// the local game user settings is enough to tell installs apart and
	// carries nothing about the player or the device.
	FString Saved;
	if (GConfig && GConfig->GetString(DeviceIdSection, DeviceIdKey, Saved, GGameUserSettingsIni) && !Saved.IsEmpty())
	{
		return Saved;
	}

	const FString Generated = TEXT("dev_") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	if (GConfig)
	{
		GConfig->SetString(DeviceIdSection, DeviceIdKey, *Generated, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	return Generated;
}

void URavensightSubsystem::Log(const FString& Message) const
{
	if (bVerboseLogging)
	{
		UE_LOG(LogRavensight, Log, TEXT("%s"), *Message);
	}
	else
	{
		UE_LOG(LogRavensight, Verbose, TEXT("%s"), *Message);
	}
}

TSharedPtr<FJsonObject> URavensightSubsystem::ParseJsonObject(const FString& Body)
{
	if (Body.IsEmpty())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
	if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
	{
		return Object;
	}

	return nullptr;
}

FString URavensightSubsystem::ExtractError(const FString& Body, const FString& Fallback)
{
	const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(Body);
	FString Error;
	if (Parsed.IsValid() && Parsed->TryGetStringField(TEXT("error"), Error) && !Error.IsEmpty())
	{
		return Error;
	}

	return Fallback;
}

double URavensightSubsystem::ParseRetryAfterSeconds(const FHttpResponsePtr& Response)
{
	if (!Response.IsValid())
	{
		return -1.0;
	}

	// Only the delta seconds form is handled. The HTTP date form is legal but
	// the API does not send it, and guessing at clock skew would be worse than
	// falling back to our own backoff.
	const FString Header = Response->GetHeader(TEXT("Retry-After")).TrimStartAndEnd();
	if (Header.IsEmpty() || !Header.IsNumeric())
	{
		return -1.0;
	}

	const double Seconds = FCString::Atod(*Header);
	return Seconds > 0.0 ? Seconds : -1.0;
}

FString URavensightSubsystem::ResponseBody(const FHttpResponsePtr& Response)
{
	return Response.IsValid() ? Response->GetContentAsString() : FString();
}
