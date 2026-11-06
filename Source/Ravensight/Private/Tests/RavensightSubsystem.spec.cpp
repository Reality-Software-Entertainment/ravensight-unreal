// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automation spec for URavensightSubsystem itself: a real game instance, a
// real subsystem, and the transport seam in RavensightTransport.h in place of
// the network. Every request the subsystem makes is recorded instead of sent,
// so a case can count requests, read their verb, URL, headers and body, and
// answer them with a fake response.
//
//   UnrealEditor-Cmd <YourProject>.uproject -ExecCmds="Automation RunTests Ravensight.Subsystem; Quit" -unattended -nopause -nullrhi
//
// The project's Ravensight settings and the saved [Ravensight] keys in
// GameUserSettings.ini are put back after every case.
//
// The fake response implements IHttpResponse as Unreal Engine 5.8 declares
// it. That interface changes between engine versions, so this spec only
// compiles on 5.8 or newer. The other two specs have no such limit.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"

#if WITH_DEV_AUTOMATION_TESTS && !UE_VERSION_OLDER_THAN(5, 8, 0)

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Interfaces/IHttpRequest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RavensightSettings.h"
#include "RavensightSpecListener.h"
#include "RavensightSubsystem.h"
#include "RavensightTransport.h"
#include "RavensightLogCapture.h"

namespace
{
	const TCHAR* const SpecKey = TEXT("gt_live_spec_key");
	const TCHAR* const SaveSection = TEXT("Ravensight");
	const TCHAR* const TrackingEnabledKey = TEXT("TrackingEnabled");
	const TCHAR* const DeviceIdKey = TEXT("DeviceId");
	const TCHAR* const OptInKey = TEXT("OptIn");

	/** A canned answer, as the HTTP module would hand it to a completion delegate. */
	class FRavensightFakeResponse final : public IHttpResponse
	{
	public:
		FRavensightFakeResponse(int32 InCode, const FString& InBody)
			: Code(InCode)
			, Body(InBody)
		{
			const FTCHARToUTF8 Utf8(*Body);
			Content.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}

		virtual const FString& GetURL() const override { return Url; }
		virtual const FString& GetEffectiveURL() const override { return Url; }
		virtual EHttpRequestStatus::Type GetStatus() const override { return EHttpRequestStatus::Succeeded; }
		virtual EHttpFailureReason GetFailureReason() const override { return EHttpFailureReason::None; }
		virtual FString GetURLParameter(const FString& /*ParameterName*/) const override { return FString(); }
		virtual FString GetHeader(const FString& /*HeaderName*/) const override { return FString(); }
		virtual TArray<FString> GetAllHeaders() const override { return TArray<FString>(); }
		virtual FString GetContentType() const override { return TEXT("application/json"); }
		virtual uint64 GetContentLength() const override { return static_cast<uint64>(Content.Num()); }
		virtual const TArray<uint8>& GetContent() const override { return Content; }
		virtual TArray<uint8> TakeContent() override { return MoveTemp(Content); }
		virtual int32 GetResponseCode() const override { return Code; }
		virtual FString GetContentAsString() const override { return Body; }
		virtual FUtf8StringView GetContentAsUtf8StringView() const override
		{
			return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Content.GetData()), Content.Num());
		}

	private:
		int32 Code = 0;
		FString Body;
		FString Url;
		TArray<uint8> Content;
	};

	FString BodyOf(const FHttpRequestPtr& Request)
	{
		const TArray<uint8>& Bytes = Request->GetContent();
		const auto Text = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
	}

	TSharedPtr<FJsonObject> JsonOf(const FHttpRequestPtr& Request)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(BodyOf(Request));
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	/** The event names of a batch request, in order. */
	TArray<FString> EventNamesOf(const FHttpRequestPtr& Request)
	{
		TArray<FString> Names;
		const TSharedPtr<FJsonObject> Body = JsonOf(Request);
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (Body.IsValid() && Body->TryGetArrayField(TEXT("events"), Events) && Events)
		{
			for (const TSharedPtr<FJsonValue>& Event : *Events)
			{
				Names.Add(Event->AsObject()->GetStringField(TEXT("event")));
			}
		}
		return Names;
	}

	/** The rs_event_id of each event in a batch request, in order. */
	TArray<FString> EventIdsOf(const FHttpRequestPtr& Request)
	{
		TArray<FString> Ids;
		const TSharedPtr<FJsonObject> Body = JsonOf(Request);
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (Body.IsValid() && Body->TryGetArrayField(TEXT("events"), Events) && Events)
		{
			for (const TSharedPtr<FJsonValue>& Event : *Events)
			{
				FString Id;
				Event->AsObject()->GetObjectField(TEXT("data"))->TryGetStringField(TEXT("rs_event_id"), Id);
				Ids.Add(Id);
			}
		}
		return Ids;
	}

	/** The data of every event in a batch request with this name, in order. */
	TArray<TSharedPtr<FJsonObject>> EventDataNamed(const FHttpRequestPtr& Request, const FString& Name)
	{
		TArray<TSharedPtr<FJsonObject>> Found;
		const TSharedPtr<FJsonObject> Body = JsonOf(Request);
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (Body.IsValid() && Body->TryGetArrayField(TEXT("events"), Events) && Events)
		{
			for (const TSharedPtr<FJsonValue>& Event : *Events)
			{
				if (Event->AsObject()->GetStringField(TEXT("event")) == Name)
				{
					Found.Add(Event->AsObject()->GetObjectField(TEXT("data")));
				}
			}
		}
		return Found;
	}

	/** Every batch request among Requests, in order. */
	TArray<FHttpRequestPtr> Batches(const TArray<FHttpRequestPtr>& All)
	{
		TArray<FHttpRequestPtr> Found;
		for (const FHttpRequestPtr& Request : All)
		{
			if (Request->GetURL().EndsWith(TEXT("/track/batch")))
			{
				Found.Add(Request);
			}
		}
		return Found;
	}

	/** The last batch request among Requests, or null. */
	FHttpRequestPtr LastBatch(const TArray<FHttpRequestPtr>& All)
	{
		for (int32 Index = All.Num() - 1; Index >= 0; --Index)
		{
			if (All[Index]->GetURL().EndsWith(TEXT("/track/batch")))
			{
				return All[Index];
			}
		}
		return nullptr;
	}

	/** ^[A-Za-z0-9_-]{1,64}$ */
	bool IsEventIdShape(const FString& Id)
	{
		if (Id.Len() < 1 || Id.Len() > 64)
		{
			return false;
		}
		for (int32 Index = 0; Index < Id.Len(); ++Index)
		{
			const TCHAR Char = Id[Index];
			const bool bOk = (Char >= TCHAR('a') && Char <= TCHAR('z')) || (Char >= TCHAR('A') && Char <= TCHAR('Z'))
				|| (Char >= TCHAR('0') && Char <= TCHAR('9')) || Char == TCHAR('_') || Char == TCHAR('-');
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	bool HasSavedKey(const TCHAR* Key)
	{
		FString Value;
		return GConfig->GetString(SaveSection, Key, Value, GGameUserSettingsIni) && !Value.IsEmpty();
	}
}

BEGIN_DEFINE_SPEC(
	FRavensightSubsystemSpec,
	"Ravensight.Subsystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/** Every request the subsystem tried to send, in order. None of them reached the network. */
	TArray<FHttpRequestPtr> Requests;
	TArray<FHttpRequestPtr> ConfigRequests;

	TArray<UGameInstance*> GameInstances;
	URavensightSpecListener* Listener = nullptr;

	/** The project's settings, put back in AfterEach. */
	bool bSavedAutoInitialize = true;
	bool bSavedEnabled = true;
	bool bSavedTrackLifecycleEvents = true;
	float SavedFlushIntervalSeconds = 5.0f;
	FString SavedIngestKey;
	FString SavedDistribution;
	bool bSavedOptional = false;
	ERavensightConsent SavedConsent = ERavensightConsent::OptOut;
	bool bSavedErrorCapture = true;

	/** The saved [Ravensight] keys, put back in AfterEach. */
	bool bHadTrackingEnabled = false;
	bool bSavedTrackingEnabled = true;
	bool bHadDeviceId = false;
	FString SavedDeviceId;
	bool bHadOptIn = false;
	bool bSavedOptIn = false;

	/** Init, then answers the settings check and the session as a healthy server would. */
	URavensightSubsystem* StartReady();

	/** A new game instance with its own new subsystem, as a launch would create it. */
	URavensightSubsystem* StartInstance();

	/** Shuts one game instance down, which deinitializes its subsystem, and destroys its world. */
	void StopInstance(URavensightSubsystem* Subsystem);

	/** Answers a recorded request as the server would. */
	void Answer(const FHttpRequestPtr& Request, int32 Code, const FString& Body);

	/** Binds the listener to a subsystem's feedback and suggestion delegates. */
	void Listen(URavensightSubsystem* Subsystem);

	FString SavedDeviceIdNow() const;

	/** Accepts every batch sent so far, flushes, accepts again, and returns the data of every "error" event sent. */
	TArray<TSharedPtr<FJsonObject>> FlushErrors(URavensightSubsystem* Subsystem);

END_DEFINE_SPEC(FRavensightSubsystemSpec)

URavensightSubsystem* FRavensightSubsystemSpec::StartInstance()
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	GameInstance->InitializeStandalone();
	GameInstances.Add(GameInstance);
	return GameInstance->GetSubsystem<URavensightSubsystem>();
}

void FRavensightSubsystemSpec::StopInstance(URavensightSubsystem* Subsystem)
{
	UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
	if (!GameInstance || !GameInstances.Contains(GameInstance))
	{
		return;
	}

	GameInstances.Remove(GameInstance);
	UWorld* World = GameInstance->GetWorld();
	GameInstance->Shutdown();
	if (World)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	GameInstance->RemoveFromRoot();
}

void FRavensightSubsystemSpec::Answer(const FHttpRequestPtr& Request, int32 Code, const FString& Body)
{
	const FHttpResponsePtr Response = MakeShared<FRavensightFakeResponse>(Code, Body);
	Request->OnProcessRequestComplete().ExecuteIfBound(Request, Response, true);
}

void FRavensightSubsystemSpec::Listen(URavensightSubsystem* Subsystem)
{
	Subsystem->OnFeedbackResult.AddDynamic(Listener, &URavensightSpecListener::HandleFeedbackResult);
	Subsystem->OnSuggestionsReceived.AddDynamic(Listener, &URavensightSpecListener::HandleSuggestions);
}

URavensightSubsystem* FRavensightSubsystemSpec::StartReady()
{
	URavensightSubsystem* Subsystem = StartInstance();
	Subsystem->Init(SpecKey);
	if (Requests.Num() >= 1)
	{
		Answer(Requests.Last(), 200, TEXT("{\"trackingEnabled\":true}"));
	}
	if (Requests.Num() >= 2)
	{
		Answer(Requests.Last(), 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));
	}
	return Subsystem;
}

TArray<TSharedPtr<FJsonObject>> FRavensightSubsystemSpec::FlushErrors(URavensightSubsystem* Subsystem)
{
	TArray<TSharedPtr<FJsonObject>> Errors;
	TSet<IHttpRequest*> Seen;
	for (int32 Round = 0; Round < 3; ++Round)
	{
		for (const FHttpRequestPtr& Batch : Batches(Requests))
		{
			if (Seen.Contains(Batch.Get()))
			{
				continue;
			}
			Seen.Add(Batch.Get());
			Errors.Append(EventDataNamed(Batch, TEXT("error")));
			Answer(Batch, 202, TEXT("{\"accepted\":1}"));
		}
		Subsystem->Flush();
	}
	return Errors;
}

FString FRavensightSubsystemSpec::SavedDeviceIdNow() const
{
	FString Value;
	GConfig->GetString(SaveSection, DeviceIdKey, Value, GGameUserSettingsIni);
	return Value;
}

void FRavensightSubsystemSpec::Define()
{
	BeforeEach([this]()
	{
		Requests.Reset();
		ConfigRequests.Reset();
		RavensightTransport::SetSenderForTests([this](const FHttpRequestRef& Request)
		{
			if (Request->GetURL().Contains(TEXT("/ingest/config?"))) ConfigRequests.Add(Request);
			else Requests.Add(Request);
			return true;
		});

		// Whatever the host project configured, every case starts the SDK
		// itself with Init, enabled, with lifecycle events on.
		URavensightSettings* Settings = GetMutableDefault<URavensightSettings>();
		bSavedAutoInitialize = Settings->bAutoInitialize;
		bSavedEnabled = Settings->bEnabled;
		bSavedTrackLifecycleEvents = Settings->bTrackLifecycleEvents;
		SavedFlushIntervalSeconds = Settings->FlushIntervalSeconds;
		SavedIngestKey = Settings->IngestKey;
		SavedDistribution = Settings->Distribution;
		bSavedOptional = Settings->bOptional;
		SavedConsent = Settings->Consent;
		bSavedErrorCapture = Settings->bErrorCapture;
		Settings->bAutoInitialize = false;
		Settings->bEnabled = true;
		Settings->bTrackLifecycleEvents = true;
		Settings->FlushIntervalSeconds = 5.0f;
		Settings->Distribution = FString();
		Settings->bOptional = false;
		Settings->Consent = ERavensightConsent::OptOut;

		// The older cases count requests and events exactly; the error
		// capture cases turn it back on themselves.
		Settings->bErrorCapture = false;

		bHadTrackingEnabled = GConfig->GetBool(SaveSection, TrackingEnabledKey, bSavedTrackingEnabled, GGameUserSettingsIni);
		bHadDeviceId = GConfig->GetString(SaveSection, DeviceIdKey, SavedDeviceId, GGameUserSettingsIni);
		bHadOptIn = GConfig->GetBool(SaveSection, OptInKey, bSavedOptIn, GGameUserSettingsIni);
		GConfig->RemoveKey(SaveSection, OptInKey, GGameUserSettingsIni);
		URavensightSubsystem::SaveTrackingEnabled(true);

		Listener = NewObject<URavensightSpecListener>();
		Listener->AddToRoot();
	});

	AfterEach([this]()
	{
		while (GameInstances.Num() > 0)
		{
			StopInstance(GameInstances.Last()->GetSubsystem<URavensightSubsystem>());
		}

		RavensightTransport::SetSenderForTests(nullptr);
		Requests.Reset();
		ConfigRequests.Reset();

		Listener->RemoveFromRoot();
		Listener = nullptr;

		URavensightSettings* Settings = GetMutableDefault<URavensightSettings>();
		Settings->bAutoInitialize = bSavedAutoInitialize;
		Settings->bEnabled = bSavedEnabled;
		Settings->bTrackLifecycleEvents = bSavedTrackLifecycleEvents;
		Settings->FlushIntervalSeconds = SavedFlushIntervalSeconds;
		Settings->IngestKey = SavedIngestKey;
		Settings->Distribution = SavedDistribution;
		Settings->bOptional = bSavedOptional;
		Settings->Consent = SavedConsent;
		Settings->bErrorCapture = bSavedErrorCapture;

		if (bHadOptIn)
		{
			GConfig->SetBool(SaveSection, OptInKey, bSavedOptIn, GGameUserSettingsIni);
		}
		else
		{
			GConfig->RemoveKey(SaveSection, OptInKey, GGameUserSettingsIni);
		}

		if (bHadTrackingEnabled)
		{
			GConfig->SetBool(SaveSection, TrackingEnabledKey, bSavedTrackingEnabled, GGameUserSettingsIni);
		}
		else
		{
			GConfig->RemoveKey(SaveSection, TrackingEnabledKey, GGameUserSettingsIni);
		}
		if (bHadDeviceId)
		{
			GConfig->SetString(SaveSection, DeviceIdKey, *SavedDeviceId, GGameUserSettingsIni);
		}
		else
		{
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);
		}
		GConfig->Flush(false, GGameUserSettingsIni);
	});

	Describe("a start with the Enabled setting off", [this]()
	{
		It("makes no request of any kind", [this]()
		{
			GetMutableDefault<URavensightSettings>()->bEnabled = false;
			URavensightSubsystem* Subsystem = StartInstance();
			Listen(Subsystem);

			Subsystem->Init(SpecKey);
			Subsystem->TrackEvent(TEXT("level_completed"), TMap<FString, FString>());
			Subsystem->Flush();
			Subsystem->FetchSuggestions();
			Subsystem->SubmitFeedback(TEXT("hello"));

			TestEqual(TEXT("requests"), Requests.Num(), 0);
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("disabled")));
			TestFalse(TEXT("tracking active"), Subsystem->IsTrackingActive());
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), 0);
		});

		It("makes its first request, the settings check, once SetEnabled(true) is called", [this]()
		{
			GetMutableDefault<URavensightSettings>()->bEnabled = false;
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			TestEqual(TEXT("requests while off"), Requests.Num(), 0);

			Subsystem->SetEnabled(true);

			if (TestEqual(TEXT("requests after SetEnabled(true)"), Requests.Num(), 1))
			{
				TestEqual(TEXT("verb"), Requests[0]->GetVerb(), FString(TEXT("GET")));
				TestEqual(TEXT("url"), Requests[0]->GetURL(), FString(TEXT("https://api.ravensight.io/api/v1/settings")));
				TestEqual(TEXT("X-API-Key"), Requests[0]->GetHeader(TEXT("X-API-Key")), FString(SpecKey));
			}
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString());
		});
	});

	Describe("a saved opt-out", [this]()
	{
		It("carries into a new subsystem instance, which makes no request", [this]()
		{
			URavensightSubsystem* First = StartInstance();
			First->Init(SpecKey);
			First->SetTrackingEnabled(false);
			TestFalse(TEXT("saved choice after opting out"), URavensightSubsystem::LoadTrackingEnabled());
			StopInstance(First);

			Requests.Reset();
		ConfigRequests.Reset();

			URavensightSubsystem* Second = StartInstance();
			Listen(Second);
			Second->Init(SpecKey);
			Second->TrackEvent(TEXT("level_completed"), TMap<FString, FString>());
			Second->Flush();
			Second->FetchSuggestions();
			Second->SubmitFeedback(TEXT("hello"));

			TestEqual(TEXT("requests from the new instance"), Requests.Num(), 0);
			TestEqual(TEXT("status reason"), Second->GetStatusReason(), FString(TEXT("disabled")));
			TestEqual(TEXT("queued"), Second->GetQueuedEventCount(), 0);
			TestEqual(TEXT("feedback reason"), Listener->LastFeedbackReason, FString(TEXT("disabled")));
			TestEqual(TEXT("suggestion calls"), Listener->SuggestionCalls, 1);
		});

		It("opting back in saves the choice and starts with a settings check", [this]()
		{
			URavensightSubsystem::SaveTrackingEnabled(false);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			TestEqual(TEXT("requests while opted out"), Requests.Num(), 0);

			Subsystem->SetTrackingEnabled(true);

			TestTrue(TEXT("saved choice"), URavensightSubsystem::LoadTrackingEnabled());
			if (TestEqual(TEXT("requests after opting in"), Requests.Num(), 1))
			{
				TestTrue(TEXT("settings check"), Requests[0]->GetURL().EndsWith(TEXT("/api/v1/settings")));
			}
		});
	});

	Describe("while tracking is off", [this]()
	{
		It("FetchSuggestions answers at once with a failure and an empty list, and sends nothing", [this]()
		{
			URavensightSubsystem::SaveTrackingEnabled(false);
			URavensightSubsystem* Subsystem = StartInstance();
			Listen(Subsystem);
			Subsystem->Init(SpecKey);

			Subsystem->FetchSuggestions();

			TestEqual(TEXT("requests"), Requests.Num(), 0);
			TestEqual(TEXT("suggestion calls"), Listener->SuggestionCalls, 1);
			TestFalse(TEXT("success"), Listener->bLastSuggestionsSuccess);
			TestEqual(TEXT("suggestions"), Listener->LastSuggestionCount, 0);
		});

		It("SubmitFeedback fails with disabled and sends nothing", [this]()
		{
			GetMutableDefault<URavensightSettings>()->bEnabled = false;
			URavensightSubsystem* Subsystem = StartInstance();
			Listen(Subsystem);
			Subsystem->Init(SpecKey);

			Subsystem->SubmitFeedback(TEXT("the boss is too hard"), TEXT("complaint"), 2);

			TestEqual(TEXT("requests"), Requests.Num(), 0);
			TestEqual(TEXT("feedback calls"), Listener->FeedbackCalls, 1);
			TestFalse(TEXT("success"), Listener->bLastFeedbackSuccess);
			TestEqual(TEXT("reason"), Listener->LastFeedbackReason, FString(TEXT("disabled")));
		});

		It("a bad category or rating still fails locally first, also while tracking is on", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Listen(Subsystem);
			Subsystem->Init(SpecKey);
			const int32 Before = Requests.Num();

			Subsystem->SubmitFeedback(TEXT("hello"), TEXT("nonsense"));
			TestEqual(TEXT("category reason"), Listener->LastFeedbackReason, FString(TEXT("invalid_category")));

			Subsystem->SubmitFeedback(TEXT("hello"), FString(), 6);
			TestEqual(TEXT("rating reason"), Listener->LastFeedbackReason, FString(TEXT("invalid_rating")));

			TestEqual(TEXT("requests added"), Requests.Num() - Before, 0);
		});
	});

	Describe("a Playtest run", [this]()
	{
		It("sends the bare token in X-Ravensight-Playtest on every request", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->SetPlaytestContext(TEXT("pt_token_123"), TEXT("run42"), TEXT("job7"), TEXT("speedrunner"));
			TestTrue(TEXT("is playtest"), Subsystem->IsPlaytest());
			Subsystem->Init(SpecKey);
			TestEqual(TEXT("device id"), Subsystem->GetDeviceId(), FString(TEXT("pt-run42")));

			if (!TestEqual(TEXT("settings request"), Requests.Num(), 1))
			{
				return;
			}
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));

			if (!TestEqual(TEXT("session request"), Requests.Num(), 2))
			{
				return;
			}
			TestTrue(TEXT("session url"), Requests[1]->GetURL().EndsWith(TEXT("/api/v1/session")));
			TestTrue(TEXT("session body device id"), BodyOf(Requests[1]).Contains(TEXT("\"deviceId\":\"pt-run42\"")));
			TestTrue(TEXT("session body platform"), BodyOf(Requests[1]).Contains(TEXT("\"platform\":\"ravensight-playtest\"")));
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_pt\",\"expiresIn\":3600}"));

			TestTrue(TEXT("session ready"), Subsystem->IsSessionReady());
			if (!TestEqual(TEXT("batch request"), Requests.Num(), 3))
			{
				return;
			}
			TestTrue(TEXT("batch url"), Requests[2]->GetURL().EndsWith(TEXT("/api/v1/track/batch")));
			TestEqual(TEXT("batch session token"), Requests[2]->GetHeader(TEXT("X-Session-Token")), FString(TEXT("tok_pt")));
			const FString BatchBody = BodyOf(Requests[2]);
			TestTrue(TEXT("game_started in the batch"), BatchBody.Contains(TEXT("\"event\":\"game_started\"")));
			TestTrue(TEXT("run tag"), BatchBody.Contains(TEXT("\"pt_run\":\"run42\"")));
			TestTrue(TEXT("synthetic tag"), BatchBody.Contains(TEXT("\"synthetic\":true")));

			for (int32 Index = 0; Index < Requests.Num(); ++Index)
			{
				TestEqual(FString::Printf(TEXT("X-Ravensight-Playtest on request %d"), Index),
					Requests[Index]->GetHeader(TEXT("X-Ravensight-Playtest")), FString(TEXT("pt_token_123")));
			}
		});

		It("ignores a saved opt-out without changing it", [this]()
		{
			URavensightSubsystem::SaveTrackingEnabled(false);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->SetPlaytestContext(TEXT("pt_token_123"), TEXT("run42"), FString(), FString());
			Subsystem->Init(SpecKey);
			Subsystem->SetTrackingEnabled(true);

			TestEqual(TEXT("settings request"), Requests.Num(), 1);
			TestFalse(TEXT("saved choice unchanged"), URavensightSubsystem::LoadTrackingEnabled());
		});

		It("is absent outside a Playtest run", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);

			TestFalse(TEXT("is playtest"), Subsystem->IsPlaytest());
			if (TestEqual(TEXT("settings request"), Requests.Num(), 1))
			{
				TestEqual(TEXT("X-Ravensight-Playtest"), Requests[0]->GetHeader(TEXT("X-Ravensight-Playtest")), FString());
			}
		});
	});

	Describe("ResetDeviceId", [this]()
	{
		It("saves a new id, empties the queue, ignores the old session's answer and opens a session under the new id", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			const FString OldId = Subsystem->GetDeviceId();
			TestTrue(TEXT("old id format"), OldId.StartsWith(TEXT("dev_")));

			if (!TestEqual(TEXT("settings request"), Requests.Num(), 1))
			{
				return;
			}
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			if (!TestEqual(TEXT("session request"), Requests.Num(), 2))
			{
				return;
			}
			const FHttpRequestPtr OldSessionRequest = Requests[1];
			TestTrue(TEXT("old session under the old id"), BodyOf(OldSessionRequest).Contains(OldId));

			Subsystem->TrackEvent(TEXT("before_reset"), TMap<FString, FString>());
			TestTrue(TEXT("queued before reset"), Subsystem->GetQueuedEventCount() > 0);

			const FString NewId = Subsystem->ResetDeviceId();

			TestNotEqual(TEXT("new id"), NewId, OldId);
			TestEqual(TEXT("GetDeviceId"), Subsystem->GetDeviceId(), NewId);
			TestEqual(TEXT("saved id"), SavedDeviceIdNow(), NewId);
			TestEqual(TEXT("queued after reset"), Subsystem->GetQueuedEventCount(), 0);

			// The answer to the session opened under the old id arrives late.
			Answer(OldSessionRequest, 201, TEXT("{\"token\":\"tok_old\",\"expiresIn\":3600}"));
			TestFalse(TEXT("old session not adopted"), Subsystem->IsSessionReady());
			TestEqual(TEXT("session token"), Subsystem->GetSessionToken(), FString());

			const int32 Before = Requests.Num();
			Subsystem->TrackEvent(TEXT("after_reset"), TMap<FString, FString>());
			Subsystem->Flush();

			if (TestEqual(TEXT("one new request"), Requests.Num() - Before, 1))
			{
				const FHttpRequestPtr NewSessionRequest = Requests.Last();
				TestTrue(TEXT("a session request"), NewSessionRequest->GetURL().EndsWith(TEXT("/api/v1/session")));
				TestTrue(TEXT("under the new id"), BodyOf(NewSessionRequest).Contains(NewId));
				TestFalse(TEXT("not under the old id"), BodyOf(NewSessionRequest).Contains(OldId));
			}
		});

		It("is ignored during a Playtest run and leaves the saved id alone", [this]()
		{
			GConfig->SetString(SaveSection, DeviceIdKey, TEXT("dev_saved_before_the_run"), GGameUserSettingsIni);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->SetPlaytestContext(TEXT("pt_token_123"), TEXT("run42"), FString(), FString());
			Subsystem->Init(SpecKey);

			TestEqual(TEXT("returned id"), Subsystem->ResetDeviceId(), FString(TEXT("pt-run42")));
			TestEqual(TEXT("saved id"), SavedDeviceIdNow(), FString(TEXT("dev_saved_before_the_run")));
		});
	});
	Describe("a start with no ingest key", [this]()
	{
		It("is inert: no request, no saved device id, every call a no-op, status no_key", [this]()
		{
			URavensightSettings* Settings = GetMutableDefault<URavensightSettings>();
			Settings->IngestKey = FString();
			Settings->bAutoInitialize = true;
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);

			URavensightSubsystem* Subsystem = StartInstance();
			Listen(Subsystem);
			Subsystem->TrackEvent(TEXT("level_completed"), TMap<FString, FString>());
			Subsystem->Flush();
			Subsystem->FetchSuggestions();
			Subsystem->SubmitFeedback(TEXT("hello"));

			TestEqual(TEXT("requests"), Requests.Num(), 0);
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("no_key")));
			TestFalse(TEXT("IsEnabled"), Subsystem->IsEnabled());
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), 0);
			TestTrue(TEXT("no device id"), Subsystem->GetDeviceId().IsEmpty());
			TestFalse(TEXT("no saved device id"), HasSavedKey(DeviceIdKey));
		});

		It("Init with an empty key is inert, and a later Init with a key starts", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->TrackEvent(TEXT("early"), TMap<FString, FString>());
			Subsystem->Init(FString());

			TestEqual(TEXT("requests while inert"), Requests.Num(), 0);
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("no_key")));
			TestEqual(TEXT("queued while inert"), Subsystem->GetQueuedEventCount(), 0);

			Subsystem->Init(SpecKey);
			if (TestEqual(TEXT("requests after a real Init"), Requests.Num(), 1))
			{
				TestTrue(TEXT("settings check"), Requests[0]->GetURL().EndsWith(TEXT("/api/v1/settings")));
			}
			TestTrue(TEXT("IsEnabled"), Subsystem->IsEnabled());
		});
	});

	Describe("Consent on Opt In", [this]()
	{
		It("before the yes makes no device id, sends nothing and drops events", [this]()
		{
			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptIn;
			GConfig->RemoveKey(SaveSection, TrackingEnabledKey, GGameUserSettingsIni);
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);

			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Subsystem->TrackEvent(TEXT("before_yes"), TMap<FString, FString>());
			Subsystem->Flush();

			TestEqual(TEXT("requests"), Requests.Num(), 0);
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), 0);
			TestTrue(TEXT("no device id"), Subsystem->GetDeviceId().IsEmpty());
			TestFalse(TEXT("no saved device id"), HasSavedKey(DeviceIdKey));
			TestFalse(TEXT("saved choice"), URavensightSubsystem::LoadTrackingEnabled());
			TestFalse(TEXT("IsEnabled"), Subsystem->IsEnabled());
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("disabled")));
		});

		It("the yes is saved, makes the id, and events right after it wait for the settings check", [this]()
		{
			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptIn;
			GConfig->RemoveKey(SaveSection, TrackingEnabledKey, GGameUserSettingsIni);
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);

			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Subsystem->SetTrackingEnabled(true);

			bool bOptIn = false;
			GConfig->GetBool(SaveSection, OptInKey, bOptIn, GGameUserSettingsIni);
			TestTrue(TEXT("OptIn saved"), bOptIn);
			TestTrue(TEXT("saved choice"), URavensightSubsystem::LoadTrackingEnabled());
			TestTrue(TEXT("device id made"), Subsystem->GetDeviceId().StartsWith(TEXT("dev_")));
			TestEqual(TEXT("device id saved"), SavedDeviceIdNow(), Subsystem->GetDeviceId());

			Subsystem->TrackEvent(TEXT("after_yes"), TMap<FString, FString>());
			if (!TestEqual(TEXT("only the settings check so far"), Requests.Num(), 1))
			{
				return;
			}
			TestEqual(TEXT("queued while the check runs"), Subsystem->GetQueuedEventCount(), 2);

			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			if (!TestEqual(TEXT("session request"), Requests.Num(), 2))
			{
				return;
			}
			TestTrue(TEXT("session under the new id"), BodyOf(Requests[1]).Contains(Subsystem->GetDeviceId()));
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_yes\",\"expiresIn\":3600}"));
			if (!TestEqual(TEXT("batch request"), Requests.Num(), 3))
			{
				return;
			}
			const TArray<FString> Names = EventNamesOf(Requests[2]);
			TestTrue(TEXT("game_started first"), Names.Num() == 2 && Names[0] == TEXT("game_started") && Names[1] == TEXT("after_yes"));
		});

		It("saying no again keeps the device id; the next launch does not read it before a yes", [this]()
		{
			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptIn;
			GConfig->RemoveKey(SaveSection, TrackingEnabledKey, GGameUserSettingsIni);
			GConfig->RemoveKey(SaveSection, DeviceIdKey, GGameUserSettingsIni);

			URavensightSubsystem* First = StartInstance();
			First->Init(SpecKey);
			First->SetTrackingEnabled(true);
			const FString Id = First->GetDeviceId();
			First->SetTrackingEnabled(false);

			TestFalse(TEXT("OptIn removed"), HasSavedKey(OptInKey));
			TestEqual(TEXT("device id kept"), SavedDeviceIdNow(), Id);
			StopInstance(First);
			Requests.Reset();
		ConfigRequests.Reset();

			URavensightSubsystem* Second = StartInstance();
			Second->Init(SpecKey);
			TestTrue(TEXT("not read before a yes"), Second->GetDeviceId().IsEmpty());
			TestEqual(TEXT("no request"), Requests.Num(), 0);

			Second->SetTrackingEnabled(true);
			TestEqual(TEXT("the same id after a new yes"), Second->GetDeviceId(), Id);
		});
	});

	Describe("the Distribution setting", [this]()
	{
		It("is sent on the session body", [this]()
		{
			GetMutableDefault<URavensightSettings>()->Distribution = TEXT("native-release");
			StartReady();
			if (TestTrue(TEXT("session request"), Requests.Num() >= 2))
			{
				TestTrue(TEXT("distribution"), BodyOf(Requests[1]).Contains(TEXT("\"distribution\":\"native-release\"")));
			}
		});

		It("is not sent when unset, too long or holding a control character", [this]()
		{
			const TArray<FString> Labels = { FString(), TEXT("12345678901234567890123456789012345678901"), TEXT("web\ndemo") };
			for (const FString& Label : Labels)
			{
				GetMutableDefault<URavensightSettings>()->Distribution = Label;
				Requests.Reset();
		ConfigRequests.Reset();
				URavensightSubsystem* Subsystem = StartReady();
				if (TestTrue(TEXT("session request"), Requests.Num() >= 2))
				{
					TestFalse(TEXT("no distribution"), BodyOf(Requests[1]).Contains(TEXT("distribution")));
				}
				StopInstance(Subsystem);
			}
		});
	});

	Describe("a refused key", [this]()
	{
		It("on the settings check ends the run: queue cleared, nothing more sent", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->OnTrackingDisabled.AddDynamic(Listener, &URavensightSpecListener::HandleTrackingDisabled);
			Subsystem->Init(SpecKey);
			Subsystem->TrackEvent(TEXT("queued_before_the_answer"), TMap<FString, FString>());
			if (!TestEqual(TEXT("settings request"), Requests.Num(), 1))
			{
				return;
			}
			Answer(Requests[0], 401, TEXT("{\"error\":\"invalid_api_key\"}"));

			Subsystem->TrackEvent(TEXT("after"), TMap<FString, FString>());
			Subsystem->Flush();
			Subsystem->SetTrackingEnabled(true);

			TestEqual(TEXT("requests"), Requests.Num(), 1);
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("invalid_key")));
			TestFalse(TEXT("IsEnabled"), Subsystem->IsEnabled());
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), 0);
			TestTrue(TEXT("tracking disabled by the key"), Listener->TrackingDisabledSources.Contains(FString(TEXT("key"))));
		});

		It("on POST /session ends the run too", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			if (!TestEqual(TEXT("session request"), Requests.Num(), 2))
			{
				return;
			}
			Answer(Requests[1], 403, TEXT("{\"error\":\"game_inactive_key\"}"));

			Subsystem->TrackEvent(TEXT("after"), TMap<FString, FString>());
			Subsystem->Flush();

			TestEqual(TEXT("requests"), Requests.Num(), 2);
			TestEqual(TEXT("status reason"), Subsystem->GetStatusReason(), FString(TEXT("invalid_key")));
			TestFalse(TEXT("IsEnabled"), Subsystem->IsEnabled());
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), 0);
		});

		It("a batch 401 still opens a new session first", [this]()
		{
			URavensightSubsystem* Subsystem = StartReady();
			if (!TestEqual(TEXT("batch request"), Requests.Num(), 3))
			{
				return;
			}
			Answer(Requests[2], 401, TEXT("{\"error\":\"invalid_session\"}"));

			if (TestEqual(TEXT("a new session request"), Requests.Num(), 4))
			{
				TestTrue(TEXT("session"), Requests[3]->GetURL().EndsWith(TEXT("/api/v1/session")));
			}
			TestTrue(TEXT("IsEnabled"), Subsystem->IsEnabled());
			TestTrue(TEXT("still queued"), Subsystem->GetQueuedEventCount() > 0);
		});
	});

	Describe("rs_event_id", [this]()
	{
		It("is on every event, well formed, unique and led by the device id", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->TrackEvent(TEXT("before_init"), TMap<FString, FString>());
			Subsystem->Init(SpecKey);
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			Subsystem->TrackEvent(TEXT("after_init"), TMap<FString, FString>());
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));
			if (!TestEqual(TEXT("batch request"), Requests.Num(), 3))
			{
				return;
			}

			const TArray<FString> Ids = EventIdsOf(Requests[2]);
			TestEqual(TEXT("three events"), Ids.Num(), 3);
			const FString Prefix = Subsystem->GetDeviceId().Left(16) + TEXT("-");
			TSet<FString> Unique;
			for (const FString& Id : Ids)
			{
				TestTrue(*FString::Printf(TEXT("shape of %s"), *Id), IsEventIdShape(Id));
				TestTrue(*FString::Printf(TEXT("prefix of %s"), *Id), Id.StartsWith(Prefix));
				Unique.Add(Id);
			}
			TestEqual(TEXT("unique"), Unique.Num(), Ids.Num());
		});

		It("a batch sent again after a failure carries the same ids", [this]()
		{
			URavensightSubsystem* Subsystem = StartReady();
			if (!TestEqual(TEXT("batch request"), Requests.Num(), 3))
			{
				return;
			}
			const TArray<FString> First = EventIdsOf(Requests[2]);
			Answer(Requests[2], 500, TEXT("{\"error\":\"internal\"}"));

			Subsystem->Flush();
			if (!TestEqual(TEXT("resent"), Requests.Num(), 4))
			{
				return;
			}
			TestTrue(TEXT("same ids"), EventIdsOf(Requests[3]) == First && First.Num() > 0);
		});
	});

	Describe("an event over 8,192 bytes", [this]()
	{
		It("is dropped before it is queued, with OnFlushFailed(event_too_large)", [this]()
		{
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->OnFlushFailed.AddDynamic(Listener, &URavensightSpecListener::HandleFlushFailed);
			Subsystem->Init(SpecKey);
			const int32 Before = Subsystem->GetQueuedEventCount();

			const FString Big = FString::ChrN(RavensightProtocol::MaxEventBytes, TCHAR('x'));
			Subsystem->TrackEventJson(TEXT("huge"), FString::Printf(TEXT("{\"blob\":\"%s\"}"), *Big));
			Subsystem->TrackEventJson(TEXT("huge_again"), FString::Printf(TEXT("{\"blob\":\"%s\"}"), *Big));
			TestEqual(TEXT("not queued"), Subsystem->GetQueuedEventCount(), Before);
			TestEqual(TEXT("two failures"), Listener->FlushFailedReasons.Num(), 2);
			TestTrue(TEXT("reason"), Listener->FlushFailedReasons.Num() > 0 && Listener->FlushFailedReasons[0] == TEXT("event_too_large"));

			const FString Fits = FString::ChrN(7000, TCHAR('y'));
			Subsystem->TrackEventJson(TEXT("fits"), FString::Printf(TEXT("{\"blob\":\"%s\"}"), *Fits));
			TestEqual(TEXT("queued"), Subsystem->GetQueuedEventCount(), Before + 1);
		});
	});
	Describe("remote configuration", [this]()
	{
		It("fetches at session start and keeps strict typed fallback values and ETags", [this]()
		{
			auto* Subsystem = StartReady();
			if (!TestEqual(TEXT("automatic fetch"), ConfigRequests.Num(), 1)) return;
			const FString Tag = FString(TEXT("\"")) + FString::ChrN(64, TCHAR('a')) + TEXT("\"");
			const auto Document = MakeShared<FJsonObject>();
			const auto Values = MakeShared<FJsonObject>(); Values->SetNumberField(TEXT("speed"), 2); Values->SetBoolField(TEXT("enabled"), true);
			Document->SetObjectField(TEXT("values"), Values); Document->SetArrayField(TEXT("experiments"), TArray<TSharedPtr<FJsonValue>>()); Document->SetStringField(TEXT("etag"), Tag);
			FString Body; const auto Writer = TJsonWriterFactory<TCHAR>::Create(&Body); FJsonSerializer::Serialize(Document, Writer);
			Answer(ConfigRequests.Last(), 200, Body);
			TestEqual(TEXT("number"), Subsystem->GetConfigNumber(TEXT("speed"), 9), 2.0);
			TestEqual(TEXT("wrong type"), Subsystem->GetConfigNumber(TEXT("enabled"), 9), 9.0);
			Subsystem->FetchConfig();
			TestEqual(TEXT("validator"), ConfigRequests.Last()->GetHeader(TEXT("If-None-Match")), Tag);
			Answer(ConfigRequests.Last(), 304, FString());
			TestEqual(TEXT("304 retains"), Subsystem->GetConfigNumber(TEXT("speed"), 9), 2.0);
			Subsystem->FetchConfig(); const auto OldRequest = ConfigRequests.Last();
			Subsystem->ResetDeviceId(); Answer(OldRequest, 200, Body);
			TestEqual(TEXT("old identity refused"), Subsystem->GetConfigNumber(TEXT("speed"), 9), 9.0);
			Subsystem->SetTrackingEnabled(false); const int32 Count = ConfigRequests.Num(); Subsystem->FetchConfig();
			TestEqual(TEXT("no opted-out request"), ConfigRequests.Num(), Count);
		});
		It("applies exposures only to new events and rejects malformed replacements", [this]()
		{
			auto* Subsystem = StartReady();
			if (!TestEqual(TEXT("fetch pending"), ConfigRequests.Num(), 1)) return;
			const FString Tag = FString(TEXT("\"")) + FString::ChrN(64, TCHAR('b')) + TEXT("\"");
			const auto Document = MakeShared<FJsonObject>(); const auto Values = MakeShared<FJsonObject>(); Values->SetNumberField(TEXT("speed"), 3);
			const auto Experiment = MakeShared<FJsonObject>(); Experiment->SetStringField(TEXT("key"), TEXT("difficulty")); Experiment->SetStringField(TEXT("variant"), TEXT("easy"));
			TArray<TSharedPtr<FJsonValue>> Assignments; Assignments.Add(MakeShared<FJsonValueObject>(Experiment));
			Document->SetObjectField(TEXT("values"), Values); Document->SetArrayField(TEXT("experiments"), Assignments); Document->SetStringField(TEXT("etag"), Tag);
			FString Body; const auto Writer = TJsonWriterFactory<TCHAR>::Create(&Body); FJsonSerializer::Serialize(Document, Writer);
			const auto InitialBatch = Requests.Last();
			const auto Initial = JsonOf(InitialBatch)->GetArrayField(TEXT("events"))[0]->AsObject()->GetObjectField(TEXT("data"));
			TestFalse(TEXT("old event unlabeled"), Initial->HasField(TEXT("rs_exp_difficulty")));
			Answer(ConfigRequests.Last(), 200, Body); Answer(InitialBatch, 202, TEXT("{}"));
			Subsystem->TrackEventJson(TEXT("after"), TEXT("{\"rs_exp_fake\":\"spoof\"}")); Subsystem->Flush();
			const auto New = JsonOf(Requests.Last())->GetArrayField(TEXT("events"))[0]->AsObject()->GetObjectField(TEXT("data"));
			TestEqual(TEXT("assignment"), New->GetStringField(TEXT("rs_exp_difficulty")), FString(TEXT("easy")));
			TestFalse(TEXT("caller label removed"), New->HasField(TEXT("rs_exp_fake")));
			Subsystem->FetchConfig(); Answer(ConfigRequests.Last(), 200, TEXT("{\"values\":{\"speed\":99}}"));
			TestEqual(TEXT("bad document retains snapshot"), Subsystem->GetConfigNumber(TEXT("speed"), 9), 3.0);
		});

		It("makes no config request without opt-in consent", [this]()
		{
			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptIn;
			auto* Subsystem = StartInstance(); Subsystem->Init(SpecKey); Subsystem->FetchConfig();
			TestEqual(TEXT("no network"), ConfigRequests.Num(), 0);
			TestEqual(TEXT("fallback"), Subsystem->GetConfigNumber(TEXT("speed"), 9), 9.0);
		});
	});

    Describe("prepared Player Reports", [this]()
    {
        It("copies screenshot bytes, uploads without credentials and retries by receipt", [this]()
        {
            auto* Subsystem = StartReady();
            TArray<uint8> Image = {137,80,78,71,0,1,2};
            const FString Handle = Subsystem->PrepareReport(TEXT("stable-report"), TEXT("Door blocked"), TEXT("bug"), Image);
            TestFalse(TEXT("prepared"), Handle.IsEmpty()); Image.Reset();
            Subsystem->SubmitReport(Handle);
            TestEqual(TEXT("create"), Requests.Last()->GetVerb(), FString(TEXT("POST")));
            TestEqual(TEXT("pinned session"), Requests.Last()->GetHeader(TEXT("X-Session-Token")), FString(TEXT("tok_spec")));
            Answer(Requests.Last(),201,TEXT("{\"id\":\"0123456789abcdef01234567\",\"status\":\"pending\"}"));
            Answer(Requests.Last(),200,TEXT("{\"url\":\"https://storage.test/\",\"fields\":{\"key\":\"staging/test\"}}"));
            TestEqual(TEXT("storage"),Requests.Last()->GetURL(),FString(TEXT("https://storage.test/")));
            TestTrue(TEXT("no session"),Requests.Last()->GetHeader(TEXT("X-Session-Token")).IsEmpty());
            TestTrue(TEXT("no ingest key"),Requests.Last()->GetHeader(TEXT("X-API-Key")).IsEmpty());
            TestTrue(TEXT("binary content"),Requests.Last()->GetContent().Contains(137));
            Answer(Requests.Last(),204,TEXT(""));
            TestTrue(TEXT("finalize"),Requests.Last()->GetURL().EndsWith(TEXT("/finalize")));
            Answer(Requests.Last(),202,TEXT("{\"id\":\"0123456789abcdef01234567\",\"status\":\"processing\"}"));
            Subsystem->SubmitReport(Handle); TestEqual(TEXT("retry polls"),Requests.Last()->GetVerb(),FString(TEXT("GET")));
            Answer(Requests.Last(),200,TEXT("{\"id\":\"0123456789abcdef01234567\",\"status\":\"complete\"}"));
            Subsystem->ResetDeviceId(); const int32 Before=Requests.Num(); Subsystem->SubmitReport(Handle);
            TestEqual(TEXT("old identity refused"),Requests.Num(),Before);
        });
        It("rejects invalid images and sends nothing while opted out", [this]()
        {
            auto* Subsystem=StartReady(); const TArray<uint8> Bad={1,2,3}; const TArray<uint8> Empty;
            TestTrue(TEXT("bad image"),Subsystem->PrepareReport(TEXT("stable-id"),TEXT("message"),TEXT("bug"),Bad).IsEmpty());
            const FString Handle=Subsystem->PrepareReport(TEXT("stable-id"),TEXT("message"),TEXT("bug"),Empty);
            Subsystem->SetTrackingEnabled(false); const int32 Before=Requests.Num(); Subsystem->SubmitReport(Handle);
            TestEqual(TEXT("no request"),Requests.Num(),Before);
        });
    });


	Describe("automatic error capture", [this]()
	{
		BeforeEach([this]()
		{
			GetMutableDefault<URavensightSettings>()->bErrorCapture = true;
		});

		// Lines go straight to GLog, as any engine or game line does once it
		// leaves GWarn. A UE_LOG Error or Warning passes the test framework's
		// filter first, which turns an expected message into a Verbose one
		// before GLog sees it, so the hook would never see it in a test.

		It("installs with a key and consent before the answer, holds a boot error and sends it behind gameplay once capture is on", [this]()
		{
			AddExpectedMessagePlain(TEXT("Boot door 7"), EAutomationExpectedMessageFlags::Contains, 0);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			TestTrue(TEXT("installed before the answer"), Subsystem->IsErrorCaptureActive());

			GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Error, TEXT("Boot door 7 failed for bob@example.com"));
			Subsystem->TrackEvent(TEXT("level_completed"), TMap<FString, FString>());

			if (!TestEqual(TEXT("settings request"), Requests.Num(), 1)) return;
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true,\"errorCapture\":{\"enabled\":true,\"trail\":false}}"));
			if (!TestEqual(TEXT("session request"), Requests.Num(), 2)) return;
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));

			const FHttpRequestPtr Batch = LastBatch(Requests);
			if (!TestTrue(TEXT("a batch went out"), Batch.IsValid())) return;
			const TArray<FString> Names = EventNamesOf(Batch);
			AddInfo(FString::Join(Names, TEXT(",")));
			TestTrue(TEXT("gameplay first"), Names.Num() == 3 && Names[0] == TEXT("game_started") && Names[1] == TEXT("level_completed") && Names[2] == TEXT("error"));

			const TArray<TSharedPtr<FJsonObject>> Errors = EventDataNamed(Batch, TEXT("error"));
			if (!TestEqual(TEXT("one error"), Errors.Num(), 1)) return;
			TestEqual(TEXT("kind"), Errors[0]->GetStringField(TEXT("rs_kind")), FString(TEXT("error")));
			TestEqual(TEXT("source"), Errors[0]->GetStringField(TEXT("rs_source")), FString(TEXT("log")));
			TestEqual(TEXT("severity"), Errors[0]->GetStringField(TEXT("rs_severity")), FString(TEXT("warning")));
			TestEqual(TEXT("type"), Errors[0]->GetStringField(TEXT("rs_error_type")), FString(TEXT("LogTemp")));
			TestEqual(TEXT("scrubbed"), Errors[0]->GetStringField(TEXT("rs_message")), FString(TEXT("Boot door 7 failed for [email]")));
			TestFalse(TEXT("no trail with the switch off"), Errors[0]->HasField(TEXT("rs_trail")));
			TestFalse(TEXT("no fingerprint from the device"), Errors[0]->HasField(TEXT("rs_fp")));
		});

		It("discards held errors unsent and uninstalls when the game's switch is off", [this]()
		{
			AddExpectedMessagePlain(TEXT("held then discarded"), EAutomationExpectedMessageFlags::Contains, 0);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Error, TEXT("held then discarded"));

			if (!TestEqual(TEXT("settings request"), Requests.Num(), 1)) return;
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true,\"errorCapture\":{\"enabled\":false,\"trail\":true}}"));
			TestFalse(TEXT("uninstalled"), Subsystem->IsErrorCaptureActive());

			GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Error, TEXT("held then discarded"));
			if (!TestEqual(TEXT("session request"), Requests.Num(), 2)) return;
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600,\"errorCapture\":{\"enabled\":false,\"trail\":false}}"));
			TestEqual(TEXT("no error sent"), FlushErrors(Subsystem).Num(), 0);
			TestFalse(TEXT("still uninstalled"), Subsystem->IsErrorCaptureActive());
		});

		It("installs nothing without a key, before an Opt In yes, or with the setting off", [this]()
		{
			URavensightSubsystem* NoKey = StartInstance();
			NoKey->Init(FString());
			TestFalse(TEXT("no key"), NoKey->IsErrorCaptureActive());

			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptIn;
			URavensightSubsystem* NoConsent = StartInstance();
			NoConsent->Init(SpecKey);
			TestFalse(TEXT("no consent"), NoConsent->IsErrorCaptureActive());
			TestEqual(TEXT("no request before consent"), Requests.Num(), 0);
			NoConsent->SetTrackingEnabled(true);
			TestTrue(TEXT("after the yes"), NoConsent->IsErrorCaptureActive());
			NoConsent->SetTrackingEnabled(false);
			TestFalse(TEXT("opt out uninstalls"), NoConsent->IsErrorCaptureActive());

			GetMutableDefault<URavensightSettings>()->Consent = ERavensightConsent::OptOut;
			GetMutableDefault<URavensightSettings>()->bErrorCapture = false;
			URavensightSubsystem* SettingOff = StartInstance();
			SettingOff->Init(SpecKey);
			TestFalse(TEXT("setting off"), SettingOff->IsErrorCaptureActive());
		});

		It("never captures its own log lines, and keeps the trail only when the switch is on", [this]()
		{
			AddExpectedMessagePlain(TEXT("own line"), EAutomationExpectedMessageFlags::Contains, 0);
			AddExpectedMessagePlain(TEXT("the real failure"), EAutomationExpectedMessageFlags::Contains, 0);
			AddExpectedMessagePlain(TEXT("low ammo"), EAutomationExpectedMessageFlags::Contains, 0);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true,\"errorCapture\":{\"enabled\":true,\"trail\":true}}"));
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));

			GLog->Log(LogRavensight.GetCategoryName(), ELogVerbosity::Error, TEXT("own line"));
			GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Warning, TEXT("low ammo"));
			GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Error, TEXT("the real failure"));

			const TArray<TSharedPtr<FJsonObject>> Errors = FlushErrors(Subsystem);
			if (!TestEqual(TEXT("one error, not its own"), Errors.Num(), 1)) return;
			TestEqual(TEXT("message"), Errors[0]->GetStringField(TEXT("rs_message")), FString(TEXT("the real failure")));
			FString Trail;
			TestTrue(TEXT("trail present"), Errors[0]->TryGetStringField(TEXT("rs_trail"), Trail));
			TestTrue(TEXT("trail holds the warning"), Trail.Contains(TEXT("warning LogTemp: low ammo")));
			TestFalse(TEXT("trail never holds its own line"), Trail.Contains(TEXT("own line")));
		});

		It("stays inside the device caps in an error storm", [this]()
		{
			AddExpectedMessagePlain(TEXT("storm "), EAutomationExpectedMessageFlags::Contains, 0);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));

			for (int32 Index = 0; Index < 200; ++Index)
			{
				GLog->Log(LogTemp.GetCategoryName(), ELogVerbosity::Error, FString::Printf(TEXT("storm %d"), Index));
			}
			TestTrue(TEXT("at most 3 of one error"), FlushErrors(Subsystem).Num() <= RavensightErrors::MaxPerKey);
		});

		It("reports an ensure once, with its stack", [this]()
		{
			// An ensure writes a block of error lines the test framework would count as failures.
			AddExpectedMessage(TEXT(".*"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 0);
			URavensightSubsystem* Subsystem = StartInstance();
			Subsystem->Init(SpecKey);
			Answer(Requests[0], 200, TEXT("{\"trackingEnabled\":true}"));
			Answer(Requests[1], 201, TEXT("{\"token\":\"tok_spec\",\"expiresIn\":3600}"));

			const bool bHeld = false;
			ensureMsgf(bHeld, TEXT("spec ensure"));

			int32 Ensures = 0;
			const TArray<TSharedPtr<FJsonObject>> Errors = FlushErrors(Subsystem);
			for (const TSharedPtr<FJsonObject>& Error : Errors)
			{
				AddInfo(Error->GetStringField(TEXT("rs_error_type")) + TEXT(": ") + Error->GetStringField(TEXT("rs_message")));
				if (Error->GetStringField(TEXT("rs_error_type")) == TEXT("Ensure"))
				{
					++Ensures;
					TestTrue(TEXT("message"), Error->GetStringField(TEXT("rs_message")).Contains(TEXT("spec ensure")));
					if (FRavensightLogCapture::HasEnsureHook())
					{
						TestTrue(TEXT("stack"), Error->HasField(TEXT("rs_stack")));
					}
				}
			}
			TestEqual(TEXT("one ensure event"), Ensures, 1);
			TestEqual(TEXT("nothing else from the ensure block"), Errors.Num(), 1);
		});
	});

}

#endif
