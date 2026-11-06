// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "RavensightSubsystem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/ScopeExit.h"
#include "RavensightBackgroundTask.h"
#include "RavensightLogCapture.h"
#include "RavensightSettings.h"
#include "RavensightTransport.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogRavensight);

namespace
{
	/** Everything the plugin keeps between runs lives under this section of GameUserSettings.ini. */
	const TCHAR* const SaveSection = TEXT("Ravensight");
	const TCHAR* const DeviceIdKey = TEXT("DeviceId");
	const TCHAR* const TrackingEnabledKey = TEXT("TrackingEnabled");

	/** Written only with the Consent setting on Opt In, once the player has said yes. */
	const TCHAR* const OptInKey = TEXT("OptIn");

	/** The per event id key inside each event's data. */
	const TCHAR* const EventIdKey = TEXT("rs_event_id");

	/** iOS and Android background marker, so a swipe killed app can report its exit next launch. */
	const TCHAR* const MarkerTokenKey = TEXT("BackgroundSessionToken");
	const TCHAR* const MarkerExpiresAtKey = TEXT("BackgroundSessionExpiresAt");
	const TCHAR* const MarkerBackgroundedAtKey = TEXT("BackgroundedAt");

	/** Session lifetime assumed when the server sends neither expiresAt nor expiresIn. */
	constexpr double DefaultSessionSeconds = 86400.0;

	/** The platform a Playtest run reports, the same on every SDK. */
	const TCHAR* const PlaytestPlatform = TEXT("ravensight-playtest");

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
	SessionTimeoutSeconds = Settings->SessionTimeoutSeconds;
	bTrackLifecycleEvents = Settings->bTrackLifecycleEvents;
	bTrackPauseEvents = Settings->bTrackPauseEvents;
	bVerboseLogging = Settings->bVerboseLogging;
	bPerformance = Settings->bPerformance;
	bErrorCapture = Settings->bErrorCapture;

	Queue.SetMaxQueueSize(Settings->MaxQueueSize);
	Queue.SetEnabled(Settings->bEnabled);
	Consent = Settings->Consent;
	StartUnixSeconds = FDateTime::UtcNow().ToUnixTimestamp();

	// The player's saved choice is read before anything else can happen, so
	// an opted out player's machine never reaches the network. A Playtest run
	// ignores it and leaves it exactly as the player set it.
	ReadPlaytestSources();
	if (!IsPlaytest())
	{
		Queue.SetLocallyEnabled(LoadTrackingEnabled());
	}

	BindApplicationDelegates();

	if (!Settings->bAutoInitialize)
	{
		Log(TEXT("auto initialize is off, waiting for an explicit Init call"));
		return;
	}

	if (Settings->IngestKey.TrimStartAndEnd().IsEmpty())
	{
		// Inert: no file written, no request, no timer, every call a no-op.
		// A public repository can ship with no key, so a clone or a
		// development build sends nothing.
		bNoKey = true;
		SetStatusReason(RavensightReason::NoKey);
		if (!Settings->bOptional)
		{
			UE_LOG(LogRavensight, Warning,
				TEXT("No ingest key, so analytics is off for this run. Fill in Project Settings > Plugins > Ravensight or call Init from code (tick Optional to silence this line)."));
		}
		return;
	}

	Init(Settings->IngestKey, Settings->ApiUrl, Settings->GameVersion);
}

void URavensightSubsystem::Deinitialize()
{
	bShuttingDown = true;

	const bool bCanSend = bInitialized && bStarted && Queue.IsActive() && !bStoppedForRun;

	// On quit (desktop) or the last chance (mobile, if the background send
	// never ran). Queued ahead of the final flush; the sampler's one send
	// rule makes a second trigger a no-op.
	if (bCanSend)
	{
		// The last lines seen before quit go out with the final flush.
		DrainErrors();
		SendHealthOnce();
	}

	if (bCanSend && bTrackLifecycleEvents && Lifecycle.TryClaimExited())
	{
		TrackEvent(TEXT("game_exited"), TMap<FString, FString>());
	}

	if (bCanSend && !bKeyRefused)
	{
		// Best effort, and bounded: the quit is held for at most 1.5 seconds
		// while a pending settings check and the last session and batch
		// requests are pumped.
		FlushInternal(true);
		PumpUntilIdle(RavensightProtocol::ExitFlushBudgetSeconds);
	}

	StopTimers();
	StopHealthSampling();
	StopErrorCapture(true);
	UnbindApplicationDelegates();
	EndBackgroundTask();

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
	return RavensightProtocol::NormalizeApiUrl(InUrl);
}

void URavensightSubsystem::Init(const FString& InIngestKey, const FString& InApiUrl, const FString& InGameVersion)
{
	if (bInitialized)
	{
		Log(TEXT("Init called again, ignoring"));
		return;
	}

	const URavensightSettings* Settings = URavensightSettings::Get();

	IngestKey = InIngestKey.TrimStartAndEnd();
	if (IngestKey.IsEmpty())
	{
		// Inert, as with an empty key in Project Settings. A later Init with
		// a key still starts the plugin.
		bNoKey = true;
		Queue.Clear();
		SetStatusReason(RavensightReason::NoKey);
		if (!Settings->bOptional)
		{
			UE_LOG(LogRavensight, Warning, TEXT("Init called with an empty ingest key. Nothing will be sent."));
		}
		return;
	}

	bNoKey = false;
	Distribution = RavensightProtocol::ValidDistribution(Settings->Distribution);
	if (Distribution.IsEmpty() && !Settings->Distribution.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("The Distribution label is over %d characters or has a control character, so it is not sent."),
			RavensightProtocol::MaxDistributionLength);
	}

	ApiUrl = NormalizeApiUrl(InApiUrl.IsEmpty() ? Settings->ApiUrl : InApiUrl);
	GameVersion = InGameVersion.IsEmpty() ? ResolveGameVersion() : InGameVersion;
	Platform = UGameplayStatics::GetPlatformName();

	bInitialized = true;

	// Process start to this call, for the health event. GStartTime is when
	// the engine started timing, which is the earliest point the plugin can see.
	BootMs = FMath::Max(0.0, (FPlatformTime::Seconds() - GStartTime) * 1000.0);
	StartHealthSampling();

	if (IsPlaytest())
	{
		if (PlaytestRunId.IsEmpty())
		{
			// Never run a Playtest token as a real player: that would put a
			// persona's events under this machine's saved identity.
			bStoppedForRun = true;
			Queue.SetServerTrackingEnabled(false);
			SetStatusReason(RavensightReason::SessionFailed);
			UE_LOG(LogRavensight, Warning,
				TEXT("A Playtest token was found without a run id. Nothing will be sent this run."));
			return;
		}

		// In memory only. The saved device id is neither read nor written.
		DeviceId = TEXT("pt-") + PlaytestRunId;
		Platform = PlaytestPlatform;
	}
	else if (Consent == ERavensightConsent::OptIn && !Queue.IsLocallyEnabled())
	{
		// Opt In before the player's yes: no device id is made or read.
		DeviceId.Reset();
	}
	else
	{
		DeviceId = ResolveDeviceId();
	}

	// Events tracked before Init were given an id without the device part,
	// since there was no device id yet. None has been sent, so add it now.
	if (!DeviceId.IsEmpty())
	{
		const FString Prefix = RavensightProtocol::EventIdPrefix(DeviceId);
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Data = ParseJsonObject(Queue.GetAt(Index).DataJson);
			FString Id;
			if (Data.IsValid() && Data->TryGetStringField(EventIdKey, Id) && Id.StartsWith(TEXT("-")))
			{
				Data->SetStringField(EventIdKey, Prefix + Id);
				Queue.ReplaceDataAt(Index, WriteJson(Data.ToSharedRef()));
			}
		}
	}

	Log(FString::Printf(TEXT("initialized against %s as %s on %s%s"),
		*ApiUrl, *GameVersion, *Platform, IsPlaytest() ? TEXT(" (Playtest run)") : TEXT("")));

	StartIfAllowed();
}

void URavensightSubsystem::StartIfAllowed()
{
	if (!bInitialized || bStoppedForRun || bShuttingDown)
	{
		return;
	}

	const FRavensightGate Gate = CurrentGate();
	if (!Gate.bAllowEnqueue)
	{
		// Nothing touches the network: no settings check, no session, no batch.
		SetStatusReason(Gate.Reason);
		return;
	}

	SetStatusReason(bKeyRefused ? FString(RavensightReason::InvalidKey) : FString());

	// With a key and consent the hooks go in now, so an error during boot is
	// held (at most five) until the settings or session answer says whether
	// the game wants them.
	StartErrorCapture();

	if (!bMarkerChecked)
	{
		bMarkerChecked = true;
		ReportMarkerExitFromLastRun();
	}

	if (!bStarted)
	{
		bStarted = true;
		StartFlushTimer();
	}

	// Queued now, at the real start time and first in the queue, ahead of
	// anything the game tracked before Init. The settings check then decides
	// whether it is ever sent.
	LoadConfigCache();
	QueueGameStartedOnce(true);

	if (!bSettingsChecked && !bSettingsRequestPending)
	{
		CheckServerSettings();
	}
}

void URavensightSubsystem::QueueGameStartedOnce(bool bAtFront)
{
	if (!bTrackLifecycleEvents || !Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	if (!Lifecycle.TryClaimStarted())
	{
		return;
	}

	EnqueueEvent(TEXT("game_started"), FString(), bAtFront);
}

FRavensightGate URavensightSubsystem::CurrentGate(bool bExplicitAttempt) const
{
	FRavensightGateInput Input;
	Input.bInitialized = bInitialized;
	Input.bEnabled = Queue.IsEnabled();
	Input.bPlayerOptedIn = Queue.IsLocallyEnabled();
	Input.bPlaytest = IsPlaytest();
	Input.bPlaytestMissingRunId = IsPlaytest() && PlaytestRunId.IsEmpty();
	Input.bStoppedForRun = bStoppedForRun || !Queue.IsServerTrackingEnabled();
	Input.bKeyRefused = bKeyRefused;
	Input.bExplicitAttempt = bExplicitAttempt;
	return RavensightProtocol::DecideGate(Input);
}

void URavensightSubsystem::SetTrackingEnabled(bool bEnabled)
{
	if (IsPlaytest())
	{
		UE_LOG(LogRavensight, Log, TEXT("SetTrackingEnabled ignored during a Playtest run; the saved choice is unchanged."));
		return;
	}

	// Written every time, before or after Init and whatever the Enabled
	// setting says, so the choice is on disk the moment the player makes it.
	SaveTrackingEnabled(bEnabled);

	if (bEnabled == Queue.IsLocallyEnabled())
	{
		return;
	}

	Queue.SetLocallyEnabled(bEnabled);

	if (!bEnabled)
	{
		Log(TEXT("tracking opted out, queue discarded"));
		StopErrorCapture(true);
		BumpQueueGeneration();
		StopTimers();
		bStarted = false;
		SetStatusReason(RavensightReason::Disabled);
		return;
	}

	if (bInitialized && DeviceId.IsEmpty())
	{
		// The first yes with Consent on Opt In: only now is an id made, or
		// the one kept from an earlier yes read back.
		DeviceId = ResolveDeviceId();
	}

	Log(TEXT("tracking opted in"));
	StartIfAllowed();
}

void URavensightSubsystem::SaveTrackingEnabled(bool bEnabled)
{
	if (!GConfig)
	{
		return;
	}

	GConfig->SetBool(SaveSection, TrackingEnabledKey, bEnabled, GGameUserSettingsIni);
	if (bEnabled && URavensightSettings::Get()->Consent == ERavensightConsent::OptIn)
	{
		GConfig->SetBool(SaveSection, OptInKey, true, GGameUserSettingsIni);
	}
	else if (!bEnabled)
	{
		// The device id is kept; ResetDeviceId forgets it.
		GConfig->RemoveKey(SaveSection, OptInKey, GGameUserSettingsIni);
	}
	GConfig->Flush(false, GGameUserSettingsIni);
}

bool URavensightSubsystem::LoadTrackingEnabled()
{
	return LoadTrackingChoice(URavensightSettings::Get()->Consent);
}

bool URavensightSubsystem::LoadTrackingChoice(ERavensightConsent InConsent)
{
	bool bSaved = true;
	if (GConfig)
	{
		GConfig->GetBool(SaveSection, TrackingEnabledKey, bSaved, GGameUserSettingsIni);
	}
	if (!bSaved || InConsent != ERavensightConsent::OptIn)
	{
		return bSaved;
	}

	bool bOptedIn = false;
	if (GConfig)
	{
		GConfig->GetBool(SaveSection, OptInKey, bOptedIn, GGameUserSettingsIni);
	}
	return bOptedIn;
}

void URavensightSubsystem::SetEnabled(bool bInEnabled)
{
	if (bInEnabled == Queue.IsEnabled())
	{
		return;
	}

	Queue.SetEnabled(bInEnabled);

	if (!bInEnabled)
	{
		Log(TEXT("disabled by the game, queue discarded"));
		StopErrorCapture(true);
		BumpQueueGeneration();
		StopTimers();
		bStarted = false;
		SetStatusReason(RavensightReason::Disabled);
		return;
	}

	Log(TEXT("enabled by the game"));
	StartIfAllowed();
}

FString URavensightSubsystem::ResetDeviceId()
{
	if (IsPlaytest())
	{
		UE_LOG(LogRavensight, Log, TEXT("ResetDeviceId ignored during a Playtest run; the saved device id is unchanged."));
		return DeviceId;
	}

	if (Consent == ERavensightConsent::OptIn && !Queue.IsLocallyEnabled())
	{
		// Opt In without a current yes: forget a saved id and make none. The
		// next yes makes a fresh one.
		if (GConfig)
		{
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
		DeviceId.Reset();
		BumpQueueGeneration();
		Queue.Clear();
		Queue.InvalidateSession();
		ErrorPipeline.ResetForNewSession(FPlatformTime::Seconds());
		Log(TEXT("device id forgotten; a new one is made on the next yes"));
		return DeviceId;
	}

	DeviceId = GenerateDeviceId();

	// Nothing recorded under the old identity is sent under the new one, and
	// a session or batch answer for the old identity is ignored.
	BumpQueueGeneration();
	Queue.Clear();
	Queue.InvalidateSession();
	ErrorPipeline.ResetForNewSession(FPlatformTime::Seconds());

	Log(TEXT("device id reset, queue discarded, next event opens a new session"));
	return DeviceId;
}

void URavensightSubsystem::SetPlaytestContext(const FString& Token, const FString& RunId, const FString& JobId, const FString& Persona)
{
	if (bInitialized)
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("SetPlaytestContext must be called before Init. Turn Auto Initialize off and call Init after it. Ignored."));
		return;
	}

	RavensightProtocol::FPlaytestSources Sources;
	Sources.ExplicitToken = Token;
	Sources.ExplicitRunId = RunId;
	const RavensightProtocol::FPlaytestResolution Resolution = RavensightProtocol::ResolvePlaytest(Sources);
	if (!Resolution.IsPlaytest())
	{
		UE_LOG(LogRavensight, Warning, TEXT("SetPlaytestContext called with an empty token. Ignored."));
		return;
	}

	PlaytestToken = Resolution.Token;
	PlaytestRunId = Resolution.RunId;
	PlaytestJobId = JobId.TrimStartAndEnd();
	PlaytestPersona = Persona.TrimStartAndEnd();

	// A Playtest run ignores the player's saved opt out without touching it.
	Queue.SetLocallyEnabled(true);
}

void URavensightSubsystem::ReadPlaytestSources()
{
	RavensightProtocol::FPlaytestSources Sources;
	Sources.EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("RAVENSIGHT_PLAYTEST_TOKEN"));
	Sources.EnvRunId = FPlatformMisc::GetEnvironmentVariable(TEXT("RAVENSIGHT_PLAYTEST_RUN_ID"));

#if PLATFORM_DESKTOP
	// --ravensight-playtest-token=... and --ravensight-playtest-run-id=...
	FParse::Value(FCommandLine::Get(), TEXT("ravensight-playtest-token="), Sources.CommandLineToken, false);
	FParse::Value(FCommandLine::Get(), TEXT("ravensight-playtest-run-id="), Sources.CommandLineRunId, false);
#endif

	const RavensightProtocol::FPlaytestResolution Resolution = RavensightProtocol::ResolvePlaytest(Sources);
	if (!Resolution.IsPlaytest())
	{
		return;
	}

	PlaytestToken = Resolution.Token;
	PlaytestRunId = Resolution.RunId;
	PlaytestJobId = FPlatformMisc::GetEnvironmentVariable(TEXT("RAVENSIGHT_PLAYTEST_JOB_ID")).TrimStartAndEnd();
	PlaytestPersona = FPlatformMisc::GetEnvironmentVariable(TEXT("RAVENSIGHT_PLAYTEST_PERSONA")).TrimStartAndEnd();
}

void URavensightSubsystem::SetStatusReason(const FString& Reason)
{
	if (Reason == StatusReason)
	{
		return;
	}

	StatusReason = Reason;
	OnStatusChanged.Broadcast(StatusReason);
}

void URavensightSubsystem::StopForRun(const FString& Reason, const FString& Source)
{
	StopHealthSampling();
	if (bStoppedForRun)
	{
		return;
	}

	bStoppedForRun = true;
	StopErrorCapture(true);
	BumpQueueGeneration();
	Queue.SetServerTrackingEnabled(false);
	StopTimers();
	SetStatusReason(Reason);

	UE_LOG(LogRavensight, Log, TEXT("Tracking stopped for this run (%s). Queue cleared, nothing more will be sent."), *Reason);
	OnTrackingDisabled.Broadcast(Source);
}

void URavensightSubsystem::RefuseKey()
{
	bKeyRefused = true;
	StopTimers();
	SetStatusReason(RavensightReason::InvalidKey);

	if (!bKeyRefusedLogged)
	{
		bKeyRefusedLogged = true;
		UE_LOG(LogRavensight, Warning,
			TEXT("Ravensight: ingest key refused (invalid_key), sending stopped for this run. Check your gt_live_ key."));
	}
}

void URavensightSubsystem::EndRunForKey()
{
	if (bStoppedForRun)
	{
		return;
	}

	bKeyRefused = true;
	if (!bKeyRefusedLogged)
	{
		bKeyRefusedLogged = true;
		UE_LOG(LogRavensight, Warning,
			TEXT("Ravensight: ingest key refused (invalid_key), sending stopped for this run. Check your gt_live_ or gt_pub_ key."));
	}

	// Queue cleared, timers stopped, nothing more sent; the next launch tries
	// the key again.
	StopForRun(RavensightReason::InvalidKey, TEXT("key"));
}

FString URavensightSubsystem::NextEventId()
{
	if (EventNonce.IsEmpty())
	{
		// The platform GUID generator, which the engine backs with the
		// system's secure random source on each platform (uuid_generate on
		// Apple, CoCreateGuid on Windows). The first 32 bits of a version 4
		// GUID are all random.
		EventNonce = FString::Printf(TEXT("%08x"), FGuid::NewGuid().A);
	}
	++EventCounter;
	return RavensightProtocol::EventId(DeviceId, StartUnixSeconds, EventNonce, EventCounter);
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
	EnqueueEvent(EventName, JsonData, false);
}

// --- Typed events -----------------------------------------------------------

namespace
{
	TArray<FRavensightProp> ExtraProps(const TMap<FString, FString>& Extra)
	{
		TArray<FRavensightProp> Props;
		for (const TPair<FString, FString>& Pair : Extra)
		{
			Props.Add(FRavensightProp::MakeString(Pair.Key, Pair.Value));
		}
		return Props;
	}
}

void URavensightSubsystem::SendTyped(const FRavensightTypedEvent& Event)
{
	TrackEventJson(Event.Name, TypedJson(Event));
}

FString URavensightSubsystem::TypedJson(const FRavensightTypedEvent& Event)
{
	// A whole number is written without a fraction, so 499 stays 499.
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	for (const FRavensightProp& Prop : Event.Props)
	{
		if (Prop.Kind == ERavensightPropKind::Number)
		{
			Payload->SetNumberField(Prop.Key, Prop.Number);
		}
		else
		{
			Payload->SetStringField(Prop.Key, Prop.Text);
		}
	}

	return WriteJson(Payload);
}

void URavensightSubsystem::TrackPurchase(int64 AmountMinor, const FString& Currency, const FString& ItemType, const FString& ItemId,
	const FString& Cart, const FString& TransactionId, const TMap<FString, FString>& Extra, const FString& EventName)
{
	SendTyped(RavensightTyped::BuildPurchase(
		false, AmountMinor, FString(), Currency, ItemType, ItemId, Cart, TransactionId, ExtraProps(Extra), EventName));
}

void URavensightSubsystem::TrackPurchaseText(const FString& AmountMinor, const FString& Currency, const FString& ItemType, const FString& ItemId,
	const FString& Cart, const FString& TransactionId, const TMap<FString, FString>& Extra, const FString& EventName)
{
	SendTyped(RavensightTyped::BuildPurchase(
		true, 0, AmountMinor, Currency, ItemType, ItemId, Cart, TransactionId, ExtraProps(Extra), EventName));
}

void URavensightSubsystem::TrackResource(const FString& Flow, const FString& Resource, double Amount, const FString& ItemType,
	const FString& ItemId, const TMap<FString, FString>& Extra, const FString& EventName)
{
	SendTyped(RavensightTyped::BuildResource(Flow, Resource, Amount, ItemType, ItemId, ExtraProps(Extra), EventName));
}

void URavensightSubsystem::TrackProgression(const FString& Status, const FString& P1, const FString& P2, const FString& P3,
	bool bHasScore, double Score, const TMap<FString, FString>& Extra, const FString& EventName)
{
	SendTyped(RavensightTyped::BuildProgression(Status, P1, P2, P3, bHasScore, Score, ExtraProps(Extra), EventName));
}

void URavensightSubsystem::TrackError(const FString& Severity, const FString& Message, const FString& Stack,
	const TMap<FString, FString>& Extra, const FString& EventName)
{
	SendTyped(RavensightTyped::BuildError(Severity, Message, Stack, ExtraProps(Extra), EventName));
}

// --- Automatic error capture ------------------------------------------------

bool URavensightSubsystem::IsErrorCaptureActive() const
{
	return LogCapture.IsValid() && LogCapture->IsInstalled();
}

void URavensightSubsystem::StartErrorCapture()
{
	if (!bErrorCapture || !bInitialized || bNoKey || bStoppedForRun || bShuttingDown
		|| ErrorPipeline.GetAnswer() == ERavensightCaptureAnswer::Off || !CurrentGate().bAllowEnqueue)
	{
		return;
	}

	if (!bErrorPipelineStarted)
	{
		bErrorPipelineStarted = true;
		ErrorPipeline.Start(FPlatformTime::Seconds());
	}

	if (!LogCapture.IsValid())
	{
		LogCapture = MakeShared<FRavensightLogCapture>(FGenericPlatformHttp::GetUrlDomain(ApiUrl));
	}
	LogCapture->SetKeepTrailLines(ErrorPipeline.IsTrailOn());
	LogCapture->Install();

	if (!ErrorTickerHandle.IsValid())
	{
		// Drained on the game thread a few times a second; the hooks only copy.
		ErrorTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &URavensightSubsystem::OnErrorTick), 0.25f);
	}
}

void URavensightSubsystem::StopErrorCapture(bool bForget)
{
	if (ErrorTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ErrorTickerHandle);
		ErrorTickerHandle.Reset();
	}

	if (LogCapture.IsValid())
	{
		LogCapture->Uninstall();
		if (bForget)
		{
			TArray<FRavensightRawCapture> Discarded;
			LogCapture->Drain(Discarded);
		}
	}

	if (bForget)
	{
		ErrorPipeline.ResetForNewSession(FPlatformTime::Seconds());
	}
}

void URavensightSubsystem::ApplyErrorCaptureAnswer(const TSharedPtr<FJsonObject>& Answer)
{
	// Absent means on; the trail is off unless the game turned it on.
	bool bOn = true;
	bool bTrail = false;
	const TSharedPtr<FJsonObject>* Capture = nullptr;
	if (Answer.IsValid() && Answer->TryGetObjectField(TEXT("errorCapture"), Capture) && Capture && Capture->IsValid())
	{
		(*Capture)->TryGetBoolField(TEXT("enabled"), bOn);
		(*Capture)->TryGetBoolField(TEXT("trail"), bTrail);
	}
	else if (ErrorPipeline.GetAnswer() != ERavensightCaptureAnswer::Unknown)
	{
		// An answer without the switches (the session after the settings
		// check) keeps what the last answer said.
		return;
	}

	// Whatever the hooks saw before the answer is judged under it.
	if (LogCapture.IsValid() && bOn)
	{
		DrainErrors();
	}

	const TArray<FRavensightTypedEvent> Ready = ErrorPipeline.Answer(bOn, bTrail);
	if (!bOn)
	{
		Log(TEXT("error capture is off for this game"));
		StopErrorCapture(false);
		if (LogCapture.IsValid())
		{
			TArray<FRavensightRawCapture> Discarded;
			LogCapture->Drain(Discarded);
		}
		return;
	}

	for (const FRavensightTypedEvent& Event : Ready)
	{
		EnqueueEvent(Event.Name, TypedJson(Event), false, true);
	}

	if (LogCapture.IsValid())
	{
		LogCapture->SetKeepTrailLines(ErrorPipeline.IsTrailOn());
	}
	StartErrorCapture();
}

void URavensightSubsystem::DrainErrors()
{
	if (!LogCapture.IsValid())
	{
		return;
	}

	TArray<FRavensightRawCapture> Records;
	LogCapture->Drain(Records);

	// Once the session's captures are spent the hooks stop copying errors at
	// all; a new session opens them again on the next drain.
	ON_SCOPE_EXIT
	{
		if (LogCapture.IsValid())
		{
			LogCapture->SetAcceptCaptures(ErrorPipeline.GetLimiter().GetCaptures() < RavensightErrors::MaxCaptures);
		}
	};
	if (Records.Num() == 0 || !Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	for (const FRavensightRawCapture& Record : Records)
	{
		FRavensightTypedEvent Event;
		const bool bReady = Record.bLine
			? ErrorPipeline.OnLine(Record.Level, Record.Category, Record.Text, Record.AtSeconds, Event)
			: ErrorPipeline.OnCapture(Record.Error, Record.AtSeconds, Event);
		if (bReady)
		{
			EnqueueEvent(Event.Name, TypedJson(Event), false, true);
		}
	}
}

bool URavensightSubsystem::OnErrorTick(float /*DeltaTime*/)
{
	if (bShuttingDown)
	{
		return false;
	}
	DrainErrors();
	return true;
}

// --- Health sampling --------------------------------------------------------

void URavensightSubsystem::StartHealthSampling()
{
	if (!bPerformance || HealthTickerHandle.IsValid())
	{
		return;
	}

	HealthTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &URavensightSubsystem::OnHealthTick), 1.0f);
}

void URavensightSubsystem::StopHealthSampling()
{
	if (HealthTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HealthTickerHandle);
		HealthTickerHandle.Reset();
	}
}

bool URavensightSubsystem::OnHealthTick(float /*DeltaTime*/)
{
	if (bShuttingDown)
	{
		return false;
	}

	// Sample only while events would actually be sent, and never send from here.
	if (Queue.IsActive() && !bStoppedForRun && !bKeyRefused)
	{
		// The last frame's time, read once a second.
		const double FrameSeconds = FApp::GetDeltaTime();
		if (FrameSeconds > 0.0)
		{
			HealthSampler.AddSample(1.0 / FrameSeconds, FMath::FRand());
		}
	}

	return true;
}

void URavensightSubsystem::SendHealthOnce()
{
	if (!bPerformance || !bInitialized || !Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	FRavensightHealthSummary Summary;
	if (!HealthSampler.TryClaimSend(Summary))
	{
		return;
	}

	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	const double MemoryMb = static_cast<double>(MemoryStats.UsedPhysical) / (1024.0 * 1024.0);

	SendTyped(RavensightTyped::BuildHealth(
		Summary.FpsMedian, Summary.FpsP10, MemoryMb > 0.0, MemoryMb, BootMs > 0.0, BootMs));
}

void URavensightSubsystem::EnqueueEvent(const FString& EventName, const FString& JsonData, bool bAtFront, bool bError)
{
	if (EventName.IsEmpty())
	{
		UE_LOG(LogRavensight, Warning, TEXT("TrackEvent called with an empty event name, ignoring"));
		return;
	}

	if (!Queue.IsActive() || bStoppedForRun || bNoKey)
	{
		return;
	}

	FString DataJson = JsonData.TrimStartAndEnd();
	TSharedPtr<FJsonObject> Parsed;
	if (!DataJson.IsEmpty())
	{
		Parsed = ParseJsonObject(DataJson);
		if (!Parsed.IsValid())
		{
			UE_LOG(LogRavensight, Warning,
				TEXT("Event '%s' carried a payload that is not a JSON object, sending it with no data"), *EventName);
		}
	}

	if (!Parsed.IsValid())
	{
		Parsed = MakeShared<FJsonObject>();
	}

	if (IsPlaytest())
	{
		// The same synthetic tags every SDK puts on a Playtest run's events.
		Parsed->SetBoolField(TEXT("synthetic"), true);
		Parsed->SetStringField(TEXT("persona"), PlaytestPersona);
		Parsed->SetStringField(TEXT("pt_job"), PlaytestJobId);
		Parsed->SetStringField(TEXT("pt_run"), PlaytestRunId);
		Parsed->SetStringField(TEXT("pt_source"), TEXT("sdk"));
		Parsed->SetStringField(TEXT("pt_tier"), TEXT("T2_driven_build"));
	}

	for (auto It = Parsed->Values.CreateIterator(); It; ++It)
	{
		if (FString(*It.Key()).StartsWith(TEXT("rs_exp_"))) It.RemoveCurrent();
	}
	for (const auto& Assignment : RemoteExperiments)
	{
		Parsed->SetStringField(TEXT("rs_exp_") + Assignment.Key, Assignment.Value);
	}

	// Inside the event's data, so a batch sent again after a failure carries
	// the same id and the server can recognise it.
	Parsed->SetStringField(EventIdKey, NextEventId());
	DataJson = WriteJson(Parsed.ToSharedRef());

	// The size the server checks: the data as JSON, in UTF-8 bytes. Bigger
	// events are dropped here rather than refused with a 400 later.
	const FTCHARToUTF8 Utf8Data(*DataJson);
	if (Utf8Data.Length() > RavensightProtocol::MaxEventBytes)
	{
		if (bError)
		{
			// An automatic capture is dropped silently: no log line, which
			// would only be captured again.
			return;
		}
		if (!bTooLargeLogged)
		{
			bTooLargeLogged = true;
			UE_LOG(LogRavensight, Warning,
				TEXT("Dropped event '%s': its data is larger than %d bytes as JSON (further drops are not logged)."),
				*EventName, RavensightProtocol::MaxEventBytes);
		}
		OnFlushFailed.Broadcast(TEXT("event_too_large"));
		return;
	}

	const int64 Timestamp = FDateTime::UtcNow().ToUnixTimestamp();
	const FRavensightEvent Event(EventName, DataJson, Timestamp, bError);
	const int32 Dropped = bAtFront ? Queue.EnqueueAtFront(Event) : Queue.Enqueue(Event);
	if (Dropped > 0)
	{
		UE_LOG(LogRavensight, Warning,
			TEXT("Queue is full at %d events, dropped %d oldest to make room"), Queue.GetMaxQueueSize(), Dropped);
	}

	// track only enqueues. Sending happens on the timer, on Flush, on
	// background and on quit, and straight away only once a full batch of
	// 50 is waiting.
	if (Queue.Num() >= RavensightLimits::MaxBatchSize)
	{
		FlushInternal(false);
	}
}

void URavensightSubsystem::Flush()
{
	// What the error hooks have seen so far goes out with it.
	DrainErrors();

	// An explicit Flush is the one way to make another attempt after the
	// ingest key was refused.
	FlushInternal(true, true);
}

void URavensightSubsystem::FlushInternal(bool bForce, bool bExplicit)
{
	// Held until the kill switch answers. Nothing is lost: the queue keeps
	// the events and the first flush after the answer sends them.
	if (!bInitialized || bStoppedForRun || !bSettingsChecked)
	{
		return;
	}

	if (!CurrentGate(bExplicit).bAllowBatch)
	{
		return;
	}

	const FRavensightFlushPlan Plan = Queue.BeginFlush(NowSeconds(), bForce, BatchLimit);

	switch (Plan.Decision)
	{
	case ERavensightFlushDecision::Send:
		SendBatch(Plan.Batch);
		break;

	case ERavensightFlushDecision::NeedsSession:
		EnsureSession(bExplicit);
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
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleBatchResponse, Batch.Num(), QueueGeneration);

	if (!RavensightTransport::Send(Request))
	{
		Queue.EndFlush();
		Queue.OnTransportFailure(NowSeconds());
		OnFlushFailed.Broadcast(TEXT("request_not_started"));
		ScheduleRetry();
	}
}

void URavensightSubsystem::HandleBatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 BatchSize, int32 Generation)
{
	// Whatever this answer leads to, an iOS background task ends once nothing is left in flight.
	ON_SCOPE_EXIT { EndBackgroundTaskIfIdle(); };

	Queue.EndFlush();

	// The queue this batch came from was thrown away while it was in flight:
	// removing BatchSize events now would remove the wrong ones.
	if (bStoppedForRun || Generation != QueueGeneration)
	{
		return;
	}

	const double Now = NowSeconds();
	const bool bAnswered = bConnectedSuccessfully && Response.IsValid();
	const int32 Code = bAnswered ? Response->GetResponseCode() : 0;
	const FString Body = bAnswered ? ResponseBody(Response) : FString();

	if (Code == 401)
	{
		++ConsecutiveBatchUnauthorized;
	}

	const FRavensightResponseVerdict Verdict = Classify(
		ERavensightEndpoint::Batch, Response, bConnectedSuccessfully, BatchSize, ConsecutiveBatchUnauthorized);

	const FString FailReason = bAnswered ? ExtractError(Body, FString::Printf(TEXT("http_%d"), Code)) : FString(TEXT("unreachable"));

	switch (Verdict.Action)
	{
	case ERavensightResponseAction::Accept:
		Queue.OnBatchAccepted(BatchSize);
		BatchLimit = RavensightLimits::MaxBatchSize;
		ConsecutiveBatchUnauthorized = 0;
		SetStatusReason(RavensightProtocol::NextStatusReason(StatusReason, Verdict));
		Log(FString::Printf(TEXT("sent %d events, %d still queued"), BatchSize, Queue.Num()));
		OnEventsFlushed.Broadcast(BatchSize);

		if (!Queue.IsEmpty())
		{
			// Keep draining while the server is happy to take them. During the
			// quit this is still bounded by the exit budget.
			FlushInternal(false);
		}
		return;

	case ERavensightResponseAction::RefreshSession:
		// Expired or revoked. The batch stays queued and goes back out as
		// soon as a fresh token is issued, at most twice in a row.
		Log(TEXT("session invalid or expired, refreshing"));
		Queue.OnUnauthorized();
		EnsureSession();
		return;

	case ERavensightResponseAction::HalveBatch:
		BatchLimit = FMath::Max(1, BatchSize / 2);
		Log(FString::Printf(TEXT("batch of %d refused with HTTP 400, retrying at %d"), BatchSize, BatchLimit));
		FlushInternal(false);
		return;

	case ERavensightResponseAction::DropHead:
		// One event the server will never accept. Drop it so the rest of the
		// queue is not blocked behind it for the whole run.
		Queue.DropHead(1);
		BatchLimit = RavensightLimits::MaxBatchSize;
		if (!bDroppedEventLogged)
		{
			bDroppedEventLogged = true;
			UE_LOG(LogRavensight, Warning,
				TEXT("Dropped an event the server refused with HTTP 400 (%s). Further drops are counted in GetQueue().GetStats() without a log line."),
				*FailReason);
		}
		OnFlushFailed.Broadcast(FailReason);
		FlushInternal(false);
		return;

	case ERavensightResponseAction::FatalKey:
		RefuseKey();
		OnFlushFailed.Broadcast(RavensightReason::InvalidKey);
		return;

	case ERavensightResponseAction::KillSwitch:
		StopForRun(Verdict.Reason, TEXT("server"));
		return;

	case ERavensightResponseAction::PlaytestStop:
		StopForRun(Verdict.Reason, TEXT("playtest_job_closed"));
		return;

	case ERavensightResponseAction::WaitRetryAfter:
		Queue.OnRateLimited(Now, ParseRetryAfterSeconds(Response));
		SetStatusReason(RavensightProtocol::NextStatusReason(StatusReason, Verdict));
		UE_LOG(LogRavensight, Warning, TEXT("track/batch refused for now (%s), retrying in %.0fs"),
			*FailReason, Queue.SecondsUntilNextAttempt(Now));
		OnFlushFailed.Broadcast(FailReason);
		ScheduleRetry();
		return;

	default:
		// BackOff: offline, a timeout, a 5xx, or a batch 401 past the re
		// open cap. Events are kept.
		if (Code == 401)
		{
			Queue.OnUnauthorized();
		}
		Log(FString::Printf(TEXT("batch flush failed (HTTP %d), will retry"), Code));
		Queue.OnTransportFailure(Now);
		SetStatusReason(RavensightProtocol::NextStatusReason(StatusReason, Verdict));
		OnFlushFailed.Broadcast(FailReason);
		ScheduleRetry();
		return;
	}
}

// --- Server kill switch -----------------------------------------------------

void URavensightSubsystem::CheckServerSettings()
{
	if (bSettingsRequestPending || !CurrentGate().bAllowSettings)
	{
		return;
	}

	bSettingsRequestPending = true;

	const FHttpRequestRef Request = MakeRequest(TEXT("GET"), TEXT("/settings"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSettingsResponse);

	if (!RavensightTransport::Send(Request))
	{
		bSettingsRequestPending = false;
		UE_LOG(LogRavensight, Warning, TEXT("settings check could not start, assuming tracking is enabled"));
		AfterSettingsChecked();
	}
}

void URavensightSubsystem::HandleSettingsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	// Whatever this answer leads to, an iOS background task ends once nothing is left in flight.
	ON_SCOPE_EXIT { EndBackgroundTaskIfIdle(); };

	bSettingsRequestPending = false;

	if (bStoppedForRun)
	{
		return;
	}

	const FRavensightResponseVerdict Verdict = Classify(ERavensightEndpoint::Settings, Response, bConnectedSuccessfully);

	switch (Verdict.Action)
	{
	case ERavensightResponseAction::Accept:
	{
		bool bServerEnabled = true;
		const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(ResponseBody(Response));
		if (Parsed.IsValid())
		{
			bool bParsed = false;
			if (Parsed->TryGetBoolField(TEXT("trackingEnabled"), bParsed))
			{
				bServerEnabled = bParsed;
			}
		}

		if (!bServerEnabled)
		{
			bSettingsChecked = true;
			StopForRun(RavensightReason::TrackingDisabled, TEXT("server"));
			return;
		}
		ApplyErrorCaptureAnswer(Parsed);
		break;
	}

	case ERavensightResponseAction::FatalKey:
		// A refused key is never "assume tracking is enabled", and a retry
		// cannot help: the run ends here.
		bSettingsChecked = true;
		EndRunForKey();
		return;

	case ERavensightResponseAction::KillSwitch:
		bSettingsChecked = true;
		StopForRun(Verdict.Reason, TEXT("server"));
		return;

	case ERavensightResponseAction::PlaytestStop:
		bSettingsChecked = true;
		StopForRun(Verdict.Reason, TEXT("playtest_job_closed"));
		return;

	case ERavensightResponseAction::WaitRetryAfter:
		// A 429, or a 503 with Retry-After: assume on, but nothing more goes
		// out until the server's wait has passed.
		Queue.OnRateLimited(NowSeconds(), ParseRetryAfterSeconds(Response));
		SetStatusReason(RavensightProtocol::NextStatusReason(StatusReason, Verdict));
		UE_LOG(LogRavensight, Warning, TEXT("settings check refused for now, assuming tracking is enabled and waiting %.0fs"),
			Queue.SecondsUntilNextAttempt(NowSeconds()));
		ScheduleRetry();
		break;

	default:
		// Offline or a server error. An unreachable settings endpoint must
		// not silently stop analytics, so assume on; the session request that
		// follows backs off on its own.
		UE_LOG(LogRavensight, Warning, TEXT("could not read settings, assuming tracking is enabled"));
		break;
	}

	AfterSettingsChecked();
}

void URavensightSubsystem::AfterSettingsChecked()
{
	bSettingsChecked = true;

	if (!Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	QueueGameStartedOnce();
	FlushInternal(false);
}

// --- Session ----------------------------------------------------------------

void URavensightSubsystem::EnsureSession(bool bExplicit)
{
	if (bSessionRequestPending || !bSettingsChecked || !CurrentGate(bExplicit).bAllowSession)
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
	if (!Distribution.IsEmpty())
	{
		Body->SetStringField(TEXT("distribution"), Distribution);
	}
	Body->SetStringField(TEXT("sdk"), RavensightVersion::SdkTag());

	const FHttpRequestRef Request = MakeRequest(TEXT("POST"), TEXT("/session"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->SetContentAsString(WriteJson(Body));
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSessionResponse, QueueGeneration);

	if (!RavensightTransport::Send(Request))
	{
		bSessionRequestPending = false;
		UE_LOG(LogRavensight, Warning, TEXT("session request could not start"));
		Queue.OnTransportFailure(NowSeconds());
		SetStatusReason(RavensightReason::SessionFailed);
		OnSessionFailed.Broadcast(TEXT("request_not_started"));
		ScheduleRetry();
	}
}

void URavensightSubsystem::HandleSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 Generation)
{
	// Whatever this answer leads to, an iOS background task ends once nothing is left in flight.
	ON_SCOPE_EXIT { EndBackgroundTaskIfIdle(); };

	bSessionRequestPending = false;

	// A session opened for a queue (or a device id) that has since been
	// thrown away is not adopted. The next flush opens a fresh one.
	if (bStoppedForRun || !Queue.IsActive() || Generation != QueueGeneration)
	{
		return;
	}

	const double Now = NowSeconds();
	const bool bAnswered = bConnectedSuccessfully && Response.IsValid();
	const int32 Code = bAnswered ? Response->GetResponseCode() : 0;
	const FString Body = bAnswered ? ResponseBody(Response) : FString();
	const FString FailReason = bAnswered ? ExtractError(Body, FString::Printf(TEXT("http_%d"), Code)) : FString(TEXT("unreachable"));

	const FRavensightResponseVerdict Verdict = Classify(ERavensightEndpoint::Session, Response, bConnectedSuccessfully);

	switch (Verdict.Action)
	{
	case ERavensightResponseAction::Accept:
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

			// A session opened, so the key is good after all. A refused key
			// had stopped the timer, so it comes back too.
			const bool bWasRefused = bKeyRefused;
			bKeyRefused = false;
			if (!bStarted || bWasRefused)
			{
				bStarted = true;
				StartFlushTimer();
			}

			Queue.SetSession(Token, ExpiresAt);

			// After batch 401s past the re open cap the ladder keeps climbing:
			// only a 202 resets it then.
			if (ConsecutiveBatchUnauthorized == 0)
			{
				Queue.ResetBackoff();
			}
			SetStatusReason(FString());
			ApplyErrorCaptureAnswer(Parsed);

			Log(TEXT("session ready"));
			OnSessionReady.Broadcast();
			FetchConfig();

			FlushInternal(false);
			return;
		}

		UE_LOG(LogRavensight, Warning, TEXT("session response was missing a token"));
		Queue.OnTransportFailure(Now);
		SetStatusReason(RavensightReason::SessionFailed);
		OnSessionFailed.Broadcast(TEXT("malformed_response"));
		ScheduleRetry();
		return;
	}

	case ERavensightResponseAction::FatalKey:
		OnSessionFailed.Broadcast(RavensightReason::InvalidKey);
		EndRunForKey();
		return;

	case ERavensightResponseAction::KillSwitch:
		StopForRun(Verdict.Reason, TEXT("server"));
		return;

	case ERavensightResponseAction::PlaytestStop:
		StopForRun(Verdict.Reason, TEXT("playtest_job_closed"));
		OnSessionFailed.Broadcast(FailReason);
		return;

	case ERavensightResponseAction::WaitRetryAfter:
		Queue.OnRateLimited(Now, ParseRetryAfterSeconds(Response));
		SetStatusReason(RavensightProtocol::NextStatusReason(StatusReason, Verdict));
		UE_LOG(LogRavensight, Warning, TEXT("session creation refused for now (%s), retrying in %.0fs"),
			*FailReason, Queue.SecondsUntilNextAttempt(Now));
		OnSessionFailed.Broadcast(FailReason);
		ScheduleRetry();
		return;

	default:
		Log(FString::Printf(TEXT("session creation failed (HTTP %d), will retry"), Code));
		Queue.OnTransportFailure(Now);
		SetStatusReason(RavensightProtocol::NextStatusReason(RavensightReason::SessionFailed, Verdict));
		OnSessionFailed.Broadcast(FailReason);
		ScheduleRetry();
		return;
	}
}

// --- Feedback ---------------------------------------------------------------

TArray<FString> URavensightSubsystem::GetFeedbackCategories()
{
	return RavensightProtocol::FeedbackCategories();
}

void URavensightSubsystem::SubmitFeedback(const FString& Message, const FString& FeedbackCategory, int32 Rating)
{
	// Validated locally first: a bad category or rating never costs a request.
	const FString Invalid = RavensightProtocol::ValidateFeedback(Message, FeedbackCategory, Rating);
	if (!Invalid.IsEmpty())
	{
		OnFeedbackResult.Broadcast(false, Invalid);
		return;
	}

	// Not active, including a refused key: refused locally with "disabled".
	if (!CurrentGate().bAllowFeedback)
	{
		OnFeedbackResult.Broadcast(false, RavensightReason::Disabled);
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

	if (!RavensightTransport::Send(Request))
	{
		OnFeedbackResult.Broadcast(false, TEXT("request_not_started"));
	}
}

void URavensightSubsystem::HandleFeedbackResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	const bool bAnswered = bConnectedSuccessfully && Response.IsValid();
	if (!bAnswered)
	{
		OnFeedbackResult.Broadcast(false, TEXT("unreachable"));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Reason = ExtractError(ResponseBody(Response), FString::Printf(TEXT("http_%d"), Code));
	const FRavensightResponseVerdict Verdict = Classify(ERavensightEndpoint::Feedback, Response, bConnectedSuccessfully);

	switch (Verdict.Action)
	{
	case ERavensightResponseAction::Accept:
		OnFeedbackResult.Broadcast(true, FString());
		return;

	case ERavensightResponseAction::RefreshSession:
		// Let the next call succeed rather than silently retrying a message
		// the player thinks they already sent.
		Queue.OnUnauthorized();
		EnsureSession();
		break;

	case ERavensightResponseAction::FatalKey:
		RefuseKey();
		break;

	case ERavensightResponseAction::KillSwitch:
		StopForRun(Verdict.Reason, TEXT("server"));
		break;

	case ERavensightResponseAction::PlaytestStop:
		StopForRun(Verdict.Reason, TEXT("playtest_job_closed"));
		break;

	default:
		break;
	}

	UE_LOG(LogRavensight, Warning, TEXT("feedback submission failed (HTTP %d)"), Code);
	OnFeedbackResult.Broadcast(false, Reason);
}

// --- Suggestions ------------------------------------------------------------

void URavensightSubsystem::FetchSuggestions()
{
	// While tracking is off nothing touches the network: same callback, an
	// empty list, no request.
	if (!CurrentGate().bAllowSuggestions)
	{
		OnSuggestionsReceived.Broadcast(false, TArray<FString>());
		return;
	}

	const FHttpRequestRef Request = MakeRequest(TEXT("GET"), TEXT("/agent/suggestions"));
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleSuggestionsResponse);

	if (!RavensightTransport::Send(Request))
	{
		OnSuggestionsReceived.Broadcast(false, TArray<FString>());
	}
}

void URavensightSubsystem::HandleSuggestionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	TArray<FString> Suggestions;

	const FRavensightResponseVerdict Verdict = Classify(ERavensightEndpoint::Suggestions, Response, bConnectedSuccessfully);
	if (Verdict.Action != ERavensightResponseAction::Accept)
	{
		const int32 Code = (bConnectedSuccessfully && Response.IsValid()) ? Response->GetResponseCode() : 0;

		if (Verdict.Action == ERavensightResponseAction::FatalKey)
		{
			RefuseKey();
		}
		else if (Verdict.Action == ERavensightResponseAction::KillSwitch)
		{
			StopForRun(Verdict.Reason, TEXT("server"));
		}
		else if (Verdict.Action == ERavensightResponseAction::PlaytestStop)
		{
			StopForRun(Verdict.Reason, TEXT("playtest_job_closed"));
		}

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
	return Queue.IsActive() && !bStoppedForRun && !bKeyRefused;
}

bool URavensightSubsystem::IsEnabled() const
{
	return bInitialized && IsTrackingActive();
}

int32 URavensightSubsystem::GetQueuedEventCount() const
{
	return Queue.Num();
}

FString URavensightSubsystem::GetDeviceId() const
{
	return DeviceId;
}

FString URavensightSubsystem::GetStatusReason() const
{
	return StatusReason;
}

FString URavensightSubsystem::GetSessionToken() const
{
	return Queue.GetSessionToken();
}

bool URavensightSubsystem::IsPlaytest() const
{
	return !PlaytestToken.IsEmpty();
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
	if (!GameInstance || bShuttingDown || bStoppedForRun || bKeyRefused)
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
	if (!Queue.IsActive() || bStoppedForRun || bKeyRefused)
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
		this, &URavensightSubsystem::OnApplicationWillEnterBackground);

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
	// Desktop focus loss is not an exit and sends no event. A flush is fine.
	if (!bInitialized || !Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	FlushInternal(true);
}

void URavensightSubsystem::OnApplicationWillEnterBackground()
{
	if (!bInitialized || !bStarted || !Queue.IsActive() || bStoppedForRun)
	{
		return;
	}

	if (bTrackPauseEvents)
	{
		TrackEvent(TEXT("game_paused"), TMap<FString, FString>());
	}

	// Mobile only: the session may never return, so this is the health send.
	// Desktop sends on quit. The sampler's one send rule keeps the other
	// trigger from sending a second one.
	if (IsMobilePlatform())
	{
		SendHealthOnce();
	}

	if (IsMobilePlatform())
	{
		BackgroundedAtSeconds = NowSeconds();
	}

	// The most important flush on mobile: iOS and Android can suspend or kill
	// the process without ever returning to the foreground. On iOS it runs
	// inside a background task, so the system allows it time to finish.
	BeginBackgroundTask();
	FlushInternal(true);
	EndBackgroundTaskIfIdle();

	if (IsMobilePlatform())
	{
		WriteBackgroundMarker();
	}
}

void URavensightSubsystem::OnApplicationEnteredForeground()
{
	if (!bInitialized)
	{
		return;
	}

	EndBackgroundTask();

	if (IsMobilePlatform())
	{
		// A Playtest run and an opted out player never touch the marker.
		if (!IsPlaytest() && Queue.IsLocallyEnabled())
		{
			DeleteBackgroundMarker();
		}

		const double BackgroundedAt = BackgroundedAtSeconds;
		BackgroundedAtSeconds = 0.0;

		const bool bCanSend = bStarted && Queue.IsActive() && !bStoppedForRun;
		if (bCanSend && RavensightProtocol::ShouldEndSessionAfterBackground(NowSeconds(), BackgroundedAt, SessionTimeoutSeconds))
		{
			// Thirty minutes or more away ends the session: game_exited at
			// the moment the app left, under the old token, then a new
			// session with game_started now, opened on the next flush.
			const FString OldToken = Queue.GetSessionToken();
			if (bTrackLifecycleEvents && !OldToken.IsEmpty() && !bKeyRefused)
			{
				SendExitUnderToken(OldToken, BackgroundedAt);
			}

			Queue.InvalidateSession();
			Lifecycle.ResetForNewSession();
			HealthSampler.ResetForNewSession();
			ErrorPipeline.ResetForNewSession(FPlatformTime::Seconds());
			QueueGameStartedOnce();
			return;
		}
	}

	if (bTrackPauseEvents && bStarted && Queue.IsActive() && !bStoppedForRun)
	{
		TrackEvent(TEXT("game_resumed"), TMap<FString, FString>());
	}
}

void URavensightSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	if (!bInitialized || !World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	FlushInternal(true);
}

void URavensightSubsystem::WriteBackgroundMarker()
{
	// A Playtest run persists nothing. Without a session there is nothing
	// the next launch could report.
	if (!GConfig || IsPlaytest() || !Queue.IsLocallyEnabled() || BackgroundedAtSeconds <= 0.0)
	{
		return;
	}

	const FString Token = Queue.GetSessionToken();
	if (Token.IsEmpty())
	{
		return;
	}

	GConfig->SetString(SaveSection, MarkerTokenKey, *Token, GGameUserSettingsIni);
	GConfig->SetString(SaveSection, MarkerExpiresAtKey, *FString::Printf(TEXT("%.0f"), Queue.GetSessionExpiresAt()), GGameUserSettingsIni);
	GConfig->SetString(SaveSection, MarkerBackgroundedAtKey, *FString::Printf(TEXT("%.0f"), BackgroundedAtSeconds), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void URavensightSubsystem::DeleteBackgroundMarker()
{
	if (!GConfig)
	{
		return;
	}

	FString Existing;
	if (!GConfig->GetString(SaveSection, MarkerTokenKey, Existing, GGameUserSettingsIni) || Existing.IsEmpty())
	{
		return;
	}

	// An empty token reads as "no marker".
	GConfig->SetString(SaveSection, MarkerTokenKey, TEXT(""), GGameUserSettingsIni);
	GConfig->SetString(SaveSection, MarkerExpiresAtKey, TEXT(""), GGameUserSettingsIni);
	GConfig->SetString(SaveSection, MarkerBackgroundedAtKey, TEXT(""), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void URavensightSubsystem::ReportMarkerExitFromLastRun()
{
	// A Playtest run and an opted out player never read or delete the marker.
	if (!IsMobilePlatform() || IsPlaytest() || !Queue.IsLocallyEnabled() || !GConfig)
	{
		return;
	}

	FString Token;
	FString ExpiresAtText;
	FString BackgroundedAtText;
	GConfig->GetString(SaveSection, MarkerTokenKey, Token, GGameUserSettingsIni);
	if (Token.IsEmpty())
	{
		return;
	}
	GConfig->GetString(SaveSection, MarkerExpiresAtKey, ExpiresAtText, GGameUserSettingsIni);
	GConfig->GetString(SaveSection, MarkerBackgroundedAtKey, BackgroundedAtText, GGameUserSettingsIni);

	const double ExpiresAt = FCString::Atod(*ExpiresAtText);
	const double BackgroundedAt = FCString::Atod(*BackgroundedAtText);

	// The last run went to the background and never came back, most likely
	// swiped away. Report its exit once, before anything else, then forget
	// the marker whatever the answer.
	if (bTrackLifecycleEvents && BackgroundedAt > 0.0
		&& RavensightProtocol::ShouldReportMarkerExit(NowSeconds(), Token, ExpiresAt, !Queue.IsLocallyEnabled()))
	{
		SendExitUnderToken(Token, BackgroundedAt);
	}

	DeleteBackgroundMarker();
}

void URavensightSubsystem::SendExitUnderToken(const FString& Token, double ExitedAtSeconds)
{
	const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("event"), TEXT("game_exited"));
	const TSharedRef<FJsonObject> ExitData = MakeShared<FJsonObject>();
	ExitData->SetStringField(EventIdKey, NextEventId());
	Entry->SetObjectField(TEXT("data"), ExitData);
	Entry->SetNumberField(TEXT("timestamp"), FMath::FloorToDouble(ExitedAtSeconds));

	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Add(MakeShared<FJsonValueObject>(Entry));

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("events"), EventValues);

	// One attempt, no retry, and the answer is not read: the old session is
	// over either way.
	const FHttpRequestRef Request = MakeRequest(TEXT("POST"), TEXT("/track/batch"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-Session-Token"), Token);
	Request->SetContentAsString(WriteJson(Body));
	RavensightTransport::Send(Request);

	Log(TEXT("reported game_exited for a session that ended in the background"));
}

void URavensightSubsystem::BeginBackgroundTask()
{
#if PLATFORM_IOS
	if (!bBackgroundTaskActive)
	{
		bBackgroundTaskActive = true;
		RavensightBackgroundTask::Begin();
	}
#endif
}

void URavensightSubsystem::EndBackgroundTask()
{
	if (bBackgroundTaskActive)
	{
		bBackgroundTaskActive = false;
		RavensightBackgroundTask::End();
	}
}

void URavensightSubsystem::EndBackgroundTaskIfIdle()
{
	if (bBackgroundTaskActive && !Queue.IsFlushInProgress() && !bSessionRequestPending && !bSettingsRequestPending)
	{
		EndBackgroundTask();
	}
}

void URavensightSubsystem::PumpUntilIdle(double BudgetSeconds)
{
	const double StartedAt = FPlatformTime::Seconds();
	FHttpManager& HttpManager = FHttpModule::Get().GetHttpManager();

	// Completion delegates run from the HTTP manager's tick, so tick it by
	// hand while the settings, session and batch requests settle, never past
	// the budget.
	while ((Queue.IsFlushInProgress() || bSessionRequestPending || bSettingsRequestPending)
		&& (FPlatformTime::Seconds() - StartedAt) < BudgetSeconds)
	{
		HttpManager.Tick(0.0f);
		FPlatformProcess::Sleep(0.01f);
	}
}

// --- Helpers ----------------------------------------------------------------

FHttpRequestRef URavensightSubsystem::MakeRequest(const FString& Verb, const FString& Path) const
{
	const FHttpRequestRef Request = RavensightTransport::CreateRequest();
	Request->SetVerb(Verb);
	Request->SetURL(ApiUrl + Path);
	Request->SetTimeout(RequestTimeoutSeconds);

	if (!PlaytestToken.IsEmpty())
	{
		// The bare token, on every request. The server also accepts a
		// "Bearer " prefix, but every SDK sends it bare.
		Request->SetHeader(TEXT("X-Ravensight-Playtest"), PlaytestToken);
	}

	return Request;
}

FRavensightResponseVerdict URavensightSubsystem::Classify(
	ERavensightEndpoint Endpoint,
	const FHttpResponsePtr& Response,
	bool bConnectedSuccessfully,
	int32 BatchSize,
	int32 InConsecutiveBatchUnauthorized)
{
	FRavensightResponseInput Input;
	Input.Endpoint = Endpoint;
	Input.BatchSize = BatchSize;
	Input.ConsecutiveBatchUnauthorized = InConsecutiveBatchUnauthorized;

	if (bConnectedSuccessfully && Response.IsValid())
	{
		Input.Status = Response->GetResponseCode();
		Input.Error = ExtractError(ResponseBody(Response), FString());
		Input.bHasRetryAfter = ParseRetryAfterSeconds(Response) > 0.0;
	}

	return RavensightProtocol::ClassifyResponse(Input);
}

double URavensightSubsystem::NowSeconds()
{
	return static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp());
}

bool URavensightSubsystem::IsMobilePlatform()
{
#if PLATFORM_IOS || PLATFORM_ANDROID
	return true;
#else
	return false;
#endif
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
	if (GConfig && GConfig->GetString(SaveSection, DeviceIdKey, Saved, GGameUserSettingsIni) && !Saved.IsEmpty())
	{
		// An existing saved id is never rewritten.
		return Saved;
	}

	return GenerateDeviceId();
}

FString URavensightSubsystem::GenerateDeviceId()
{
	// Same format since 0.1.0: "dev_" and a lower case GUID with hyphens.
	const FString Generated = TEXT("dev_") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	if (GConfig)
	{
		GConfig->SetString(SaveSection, DeviceIdKey, *Generated, GGameUserSettingsIni);
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

// --- Public remote configuration --------------------------------------------
namespace
{
	FString ConfigJson(const TSharedPtr<FJsonValue>& Value)
	{
		const auto Wrapper = MakeShared<FJsonObject>(); Wrapper->SetField(TEXT("v"), Value);
		const FString Wrapped = WriteJson(Wrapper);
		return Wrapped.Mid(5, Wrapped.Len() - 6);
	}
	bool SafeConfigValue(const TSharedPtr<FJsonValue>& Value, int32 Depth = 0)
	{
		if (!Value.IsValid() || Depth > 20 || (Value->Type == EJson::Number && !FMath::IsFinite(Value->AsNumber()))) return false;
		if (Value->Type == EJson::Object)
		{
			for (const auto& Pair : Value->AsObject()->Values)
			{
				if (Pair.Key == TEXT("__proto__") || Pair.Key == TEXT("constructor") || Pair.Key == TEXT("prototype") || !SafeConfigValue(Pair.Value, Depth + 1)) return false;
			}
		}
		if (Value->Type == EJson::Array)
		{
			for (const auto& Item : Value->AsArray()) if (!SafeConfigValue(Item, Depth + 1)) return false;
		}
		return true;
	}
}

void URavensightSubsystem::BumpQueueGeneration()
{
	++QueueGeneration;
	RemoteValues.Reset(); RemoteExperiments.Empty(); RemoteETag.Reset();
	bConfigRequestPending = false; bConfigCacheLoaded = false;
}

FString URavensightSubsystem::ConfigCachePath() const
{
	if (IsPlaytest() || DeviceId.IsEmpty()) return FString();
	const auto Scope = MakeShared<FJsonObject>();
	Scope->SetStringField(TEXT("endpoint"), ApiUrl); Scope->SetStringField(TEXT("key"), IngestKey);
	Scope->SetStringField(TEXT("device"), DeviceId); Scope->SetStringField(TEXT("platform"), Platform);
	Scope->SetStringField(TEXT("build"), GameVersion);
	const FString Text = WriteJson(Scope); const FTCHARToUTF8 Bytes(*Text);
	FSHAHash Hash; FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Hash.Hash);
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Ravensight"), TEXT("config_") + Hash.ToString() + TEXT(".json"));
}

void URavensightSubsystem::LoadConfigCache()
{
	if (bConfigCacheLoaded || bShuttingDown || !CurrentGate().bAllowSuggestions) return;
	bConfigCacheLoaded = true;
	const FString Path = ConfigCachePath();
	const int64 Size = Path.IsEmpty() ? -1 : IFileManager::Get().FileSize(*Path);
	FString Body;
	if (Size >= 0 && Size <= 450000 && FFileHelper::LoadFileToString(Body, *Path)) ApplyConfigDocument(Body);
}

bool URavensightSubsystem::ApplyConfigDocument(const FString& Body)
{
	if (!RavensightConfigProtocol::BoundedDocument(Body) || FTCHARToUTF8(*Body).Length() > 450000) return false;
	const auto Root = ParseJsonObject(Body);
	const TSharedPtr<FJsonObject>* Values = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Experiments = nullptr;
	FString ETag;
	if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("values"), Values) || !Values || !Values->IsValid()
		|| (*Values)->Values.Num() > 50 || !Root->TryGetArrayField(TEXT("experiments"), Experiments) || !Experiments || Experiments->Num() > 10
		|| !Root->TryGetStringField(TEXT("etag"), ETag) || !RavensightConfigProtocol::ETag(ETag)) return false;
	for (const auto& Pair : (*Values)->Values)
	{
		if (!RavensightConfigProtocol::Name(FString(*Pair.Key), 64) || Pair.Key == TEXT("constructor") || Pair.Key == TEXT("prototype") || !SafeConfigValue(Pair.Value) || FTCHARToUTF8(*ConfigJson(Pair.Value)).Length() > 8192) return false;
	}
	TMap<FString, FString> Assignments;
	for (const auto& Item : *Experiments)
	{
		if (!Item.IsValid() || Item->Type != EJson::Object) return false;
		FString Key, Variant;
		if (!Item->AsObject()->TryGetStringField(TEXT("key"), Key) || !Item->AsObject()->TryGetStringField(TEXT("variant"), Variant)
			|| !RavensightConfigProtocol::Name(Key, 64) || !RavensightConfigProtocol::Name(Variant, 40, true) || Assignments.Contains(Key)) return false;
		Assignments.Add(Key, Variant);
	}
	RemoteValues = *Values; RemoteExperiments = MoveTemp(Assignments); RemoteETag = ETag;
	return true;
}

void URavensightSubsystem::FetchConfig()
{
	if (bShuttingDown || bConfigRequestPending || !CurrentGate().bAllowSuggestions) return;
	LoadConfigCache();
	const FString Path = TEXT("/ingest/config?deviceId=") + FGenericPlatformHttp::UrlEncode(DeviceId)
		+ TEXT("&platform=") + FGenericPlatformHttp::UrlEncode(Platform) + TEXT("&gameVersion=") + FGenericPlatformHttp::UrlEncode(GameVersion);
	const FHttpRequestRef Request = MakeRequest(TEXT("GET"), Path);
	Request->SetHeader(TEXT("X-API-Key"), IngestKey);
	if (!RemoteETag.IsEmpty()) Request->SetHeader(TEXT("If-None-Match"), RemoteETag);
	bConfigRequestPending = true;
	Request->OnProcessRequestComplete().BindUObject(this, &URavensightSubsystem::HandleConfigResponse, QueueGeneration);
	if (!RavensightTransport::Send(Request)) { bConfigRequestPending = false; OnConfigReady.Broadcast(false); }
}

void URavensightSubsystem::HandleConfigResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 Generation)
{
	if (Generation != QueueGeneration || bShuttingDown || !CurrentGate().bAllowSuggestions) return;
	bConfigRequestPending = false;
	const int32 Code = bConnectedSuccessfully && Response.IsValid() ? Response->GetResponseCode() : 0;
	if (Code == 401 || Code == 403)
	{
		const auto Verdict = Classify(ERavensightEndpoint::Suggestions, Response, bConnectedSuccessfully);
		if (Verdict.Action == ERavensightResponseAction::FatalKey) { BumpQueueGeneration(); RefuseKey(); return; }
		if (Verdict.Action == ERavensightResponseAction::KillSwitch || Verdict.Action == ERavensightResponseAction::PlaytestStop) { StopForRun(Verdict.Reason, TEXT("server")); return; }
	}
	bool bFresh = Code == 304 && !RemoteETag.IsEmpty() && Request->GetHeader(TEXT("If-None-Match")) == RemoteETag;
	if (Code == 200 && ApplyConfigDocument(ResponseBody(Response)))
	{
		bFresh = true;
		const FString Path = ConfigCachePath();
		if (!Path.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			const FString Temp = Path + TEXT(".tmp");
			if (FFileHelper::SaveStringToFile(ResponseBody(Response), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) IFileManager::Get().Move(*Path, *Temp, true, true);
		}
	}
	OnConfigReady.Broadcast(bFresh);
}

TSharedPtr<FJsonValue> URavensightSubsystem::ConfigValue(const FString& Key) const
{
	if (!Queue.IsActive() || bStoppedForRun || bKeyRefused || !RemoteValues.IsValid()) return nullptr;
	return RemoteValues->TryGetField(Key);
}
bool URavensightSubsystem::GetConfigBool(const FString& Key, bool Fallback) const
{
	const auto Value = ConfigValue(Key); return Value.IsValid() && Value->Type == EJson::Boolean ? Value->AsBool() : Fallback;
}
double URavensightSubsystem::GetConfigNumber(const FString& Key, double Fallback) const
{
	const auto Value = ConfigValue(Key); return Value.IsValid() && Value->Type == EJson::Number ? Value->AsNumber() : Fallback;
}
FString URavensightSubsystem::GetConfigString(const FString& Key, const FString& Fallback) const
{
	const auto Value = ConfigValue(Key); return Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : Fallback;
}
FString URavensightSubsystem::GetConfigJson(const FString& Key, const FString& Fallback) const
{
	const auto Value = ConfigValue(Key); return Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array) ? ConfigJson(Value) : Fallback;
}

#include "RavensightReportHash.h"


FString URavensightSubsystem::PrepareReport(const FString& ClientId, const FString& Message, const FString& Category, const TArray<uint8>& Screenshot)
{
    check(IsInGameThread());
    if (PreparedReports.Num() >= 16 || ClientId.Len() < 8 || ClientId.Len() > 80 || Message.TrimStartAndEnd().IsEmpty() || Message.Len() > 500 || Screenshot.Num() > 1000000 || !RavensightProtocol::ValidateFeedback(Message, Category, 0).IsEmpty()) return FString();
    for (TCHAR C : ClientId) if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return FString();
    if (Screenshot.Num() > 0)
    {
        const bool Png = Screenshot.Num() >= 4 && Screenshot[0] == 137 && Screenshot[1] == 80 && Screenshot[2] == 78 && Screenshot[3] == 71;
        const bool Jpeg = Screenshot.Num() >= 3 && Screenshot[0] == 255 && Screenshot[1] == 216 && Screenshot[2] == 255;
        const bool Webp = Screenshot.Num() >= 12 && FMemory::Memcmp(Screenshot.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Screenshot.GetData() + 8, "WEBP", 4) == 0;
        if (!Png && !Jpeg && !Webp) return FString();
    }
    const auto Report = MakeShared<FRavensightPreparedReport>(); Report->Image = Screenshot; Report->Generation = QueueGeneration;
    const auto Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("clientId"), ClientId); Payload->SetStringField(TEXT("message"), Message.TrimStartAndEnd());
    Payload->SetStringField(TEXT("category"), Category.IsEmpty() ? TEXT("other") : Category); Payload->SetNumberField(TEXT("capturedAt"), FDateTime::UtcNow().ToUnixTimestamp());
    if (Screenshot.Num())
    {
        const std::string Bytes(reinterpret_cast<const char*>(Screenshot.GetData()), Screenshot.Num());
        Payload->SetBoolField(TEXT("image"), true); Payload->SetNumberField(TEXT("bytes"), Screenshot.Num());
        Payload->SetStringField(TEXT("sha256"), UTF8_TO_TCHAR(RavensightReportHash::sha256_hex(Bytes).c_str()));
    }
    Report->Payload = WriteJson(Payload);
    const FString Handle = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    PreparedReports.Add(Handle, Report); return Handle;
}

bool URavensightSubsystem::CanSendReport(const TSharedPtr<FRavensightPreparedReport>& Report) const
{
    return Report.IsValid() && Report->Generation == QueueGeneration && !bShuttingDown && CurrentGate().bAllowFeedback && !IsPlaytest();
}
void URavensightSubsystem::SubmitReport(const FString& Handle) { ReportCommand(Handle, TEXT("submit")); }
void URavensightSubsystem::GetReportStatus(const FString& Handle) { ReportCommand(Handle, TEXT("status")); }
void URavensightSubsystem::CancelReport(const FString& Handle) { ReportCommand(Handle, TEXT("cancel")); }
void URavensightSubsystem::ForgetReport(const FString& Handle)
{
    check(IsInGameThread()); const auto* Report = PreparedReports.Find(Handle);
    if (Report && !(*Report)->bBusy) PreparedReports.Remove(Handle);
}
void URavensightSubsystem::ReportCommand(const FString& Handle, const FString& Operation)
{
    check(IsInGameThread());
    const auto* Found = PreparedReports.Find(Handle);
    if (!Found || !CanSendReport(*Found) || (*Found)->bBusy) { OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("report_unavailable")); return; }
    const auto Report = *Found;
    if (Report->Token.IsEmpty())
    {
        if (Operation != TEXT("submit") || !Queue.IsSessionValid(NowSeconds())) { OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("no_session")); return; }
        Report->Token = Queue.GetSessionToken();
    }
    if (Operation != TEXT("submit") && Report->SubmissionId.IsEmpty()) { OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("report_not_submitted")); return; }
    Report->bBusy = true;
    SendReportStep(Handle, Report, Operation == TEXT("submit") ? (Report->SubmissionId.IsEmpty() ? TEXT("create") : TEXT("resume")) : Operation);
}
void URavensightSubsystem::SendReportStep(const FString& Handle, const TSharedPtr<FRavensightPreparedReport>& Report, const FString& Step)
{
    if (!CanSendReport(Report)) { Report->bBusy = false; OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("report_session_changed")); return; }
    FString Path = TEXT("/reports"); FString Method = TEXT("POST");
    if (Step != TEXT("create")) Path += TEXT("/") + Report->SubmissionId;
    if (Step == TEXT("resume") || Step == TEXT("status")) Method = TEXT("GET");
    else if (Step == TEXT("cancel")) Method = TEXT("DELETE");
    else if (Step == TEXT("upload") || Step == TEXT("finalize")) Path += TEXT("/") + Step;
    const auto Request = MakeRequest(Method, Path);
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json")); Request->SetHeader(TEXT("X-Session-Token"), Report->Token);
    if (Method == TEXT("POST")) Request->SetContentAsString(Step == TEXT("create") ? Report->Payload : TEXT("{}"));
    Request->OnProcessRequestComplete().BindWeakLambda(this, [this, Handle, Report, Step](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
    {
        auto Fail = [&](const FString& Error) { Report->bBusy = false; OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), Error); };
        if (!CanSendReport(Report)) { Fail(TEXT("report_session_changed")); return; }
        if (!bConnected || !Response.IsValid() || Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300) { Fail(TEXT("report_request_failed")); return; }
        TSharedPtr<FJsonObject> Body; const auto Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
        if (!FJsonSerializer::Deserialize(Reader, Body) || !Body.IsValid()) { Fail(TEXT("invalid_response")); return; }
        if (Step == TEXT("upload"))
        {
            FString Url; const TSharedPtr<FJsonObject>* Fields = nullptr;
            if (!Body->TryGetStringField(TEXT("url"), Url) || !Url.StartsWith(TEXT("https://")) || Url.Len() < 9 || Url.Contains(TEXT("@")) || Url.Contains(TEXT("\\")) || Url.Contains(TEXT("\r")) || Url.Contains(TEXT("\n")) || Url.Contains(TEXT(" ")) || Url.Contains(TEXT("\t")) || Url[8] == TCHAR('/') || !Body->TryGetObjectField(TEXT("fields"), Fields) || (*Fields)->Values.Num() > 32) { Fail(TEXT("invalid_upload")); return; }
            const FString Boundary = TEXT("rs-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            TArray<uint8> Bytes;
            auto Append = [&](const FString& Text) { const FTCHARToUTF8 Utf8(*Text); Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); };
            for (const auto& Field : (*Fields)->Values)
            {
                FString Value; const FString FieldName(*Field.Key);
                if (FieldName.IsEmpty() || FieldName.Len() > 100 || !Field.Value->TryGetString(Value) || Value.Len() > 16000) { Fail(TEXT("invalid_upload")); return; }
                for (TCHAR C : FieldName) if (!FChar::IsAlnum(C) && C != '_' && C != '-') { Fail(TEXT("invalid_upload")); return; }
                Append(TEXT("--") + Boundary + TEXT("\r\nContent-Disposition: form-data; name=\"") + FieldName + TEXT("\"\r\n\r\n") + Value + TEXT("\r\n"));
            }
            Append(TEXT("--") + Boundary + TEXT("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"screenshot\"\r\nContent-Type: application/octet-stream\r\n\r\n"));
            Bytes.Append(Report->Image); Append(TEXT("\r\n--") + Boundary + TEXT("--\r\n"));
            // Fresh request, deliberately bypasses MakeRequest: no API/playtest/session credentials.
            const auto Upload = RavensightTransport::CreateRequest(); Upload->SetURL(Url); Upload->SetVerb(TEXT("POST")); Upload->SetTimeout(30.0f);
            Upload->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary); Upload->SetContent(Bytes);
            Upload->OnProcessRequestComplete().BindWeakLambda(this, [this, Handle, Report](FHttpRequestPtr, FHttpResponsePtr Uploaded, bool bOk)
            {
                if (!CanSendReport(Report) || !bOk || !Uploaded.IsValid() || Uploaded->GetResponseCode() < 200 || Uploaded->GetResponseCode() >= 300) { Report->bBusy = false; OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("upload_failed")); return; }
                Report->bUploaded = true; SendReportStep(Handle, Report, TEXT("finalize"));
            });
            if (!RavensightTransport::Send(Upload)) Fail(TEXT("upload_not_started"));
            return;
        }
        FString Id, Status, ReportId;
        if (!Body->TryGetStringField(TEXT("id"), Id) || Id.Len() != 24 || !Body->TryGetStringField(TEXT("status"), Status) || (Status != TEXT("pending") && Status != TEXT("processing") && Status != TEXT("complete") && Status != TEXT("canceled"))) { Fail(TEXT("invalid_response")); return; }
        for (TCHAR C : Id) if (!FChar::IsHexDigit(C)) { Fail(TEXT("invalid_response")); return; }
        if (!Report->SubmissionId.IsEmpty() && Report->SubmissionId != Id) { Fail(TEXT("invalid_response")); return; }
        Report->SubmissionId = Id;
        if ((Step == TEXT("create") || Step == TEXT("resume")) && Status == TEXT("pending") && Report->Image.Num()) { SendReportStep(Handle, Report, Report->bUploaded ? TEXT("finalize") : TEXT("upload")); return; }
        Body->TryGetStringField(TEXT("reportId"), ReportId); Report->bBusy = false; OnReportResult.Broadcast(Handle, Status, ReportId, TEXT(""));
    });
    if (!RavensightTransport::Send(Request)) { Report->bBusy = false; OnReportResult.Broadcast(Handle, TEXT(""), TEXT(""), TEXT("request_not_started")); }
}
