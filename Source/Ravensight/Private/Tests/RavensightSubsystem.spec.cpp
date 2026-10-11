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
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "RavensightSettings.h"
#include "RavensightSpecListener.h"
#include "RavensightSubsystem.h"
#include "RavensightTransport.h"

namespace
{
	const TCHAR* const SpecKey = TEXT("gt_live_spec_key");
	const TCHAR* const SaveSection = TEXT("Ravensight");
	const TCHAR* const TrackingEnabledKey = TEXT("TrackingEnabled");
	const TCHAR* const DeviceIdKey = TEXT("DeviceId");

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
}

BEGIN_DEFINE_SPEC(
	FRavensightSubsystemSpec,
	"Ravensight.Subsystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/** Every request the subsystem tried to send, in order. None of them reached the network. */
	TArray<FHttpRequestPtr> Requests;

	TArray<UGameInstance*> GameInstances;
	URavensightSpecListener* Listener = nullptr;

	/** The project's settings, put back in AfterEach. */
	bool bSavedAutoInitialize = true;
	bool bSavedEnabled = true;
	bool bSavedTrackLifecycleEvents = true;
	float SavedFlushIntervalSeconds = 5.0f;

	/** The saved [Ravensight] keys, put back in AfterEach. */
	bool bHadTrackingEnabled = false;
	bool bSavedTrackingEnabled = true;
	bool bHadDeviceId = false;
	FString SavedDeviceId;

	/** A new game instance with its own new subsystem, as a launch would create it. */
	URavensightSubsystem* StartInstance();

	/** Shuts one game instance down, which deinitializes its subsystem, and destroys its world. */
	void StopInstance(URavensightSubsystem* Subsystem);

	/** Answers a recorded request as the server would. */
	void Answer(const FHttpRequestPtr& Request, int32 Code, const FString& Body);

	/** Binds the listener to a subsystem's feedback and suggestion delegates. */
	void Listen(URavensightSubsystem* Subsystem);

	FString SavedDeviceIdNow() const;

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
		RavensightTransport::SetSenderForTests([this](const FHttpRequestRef& Request)
		{
			Requests.Add(Request);
			return true;
		});

		// Whatever the host project configured, every case starts the SDK
		// itself with Init, enabled, with lifecycle events on.
		URavensightSettings* Settings = GetMutableDefault<URavensightSettings>();
		bSavedAutoInitialize = Settings->bAutoInitialize;
		bSavedEnabled = Settings->bEnabled;
		bSavedTrackLifecycleEvents = Settings->bTrackLifecycleEvents;
		SavedFlushIntervalSeconds = Settings->FlushIntervalSeconds;
		Settings->bAutoInitialize = false;
		Settings->bEnabled = true;
		Settings->bTrackLifecycleEvents = true;
		Settings->FlushIntervalSeconds = 5.0f;

		bHadTrackingEnabled = GConfig->GetBool(SaveSection, TrackingEnabledKey, bSavedTrackingEnabled, GGameUserSettingsIni);
		bHadDeviceId = GConfig->GetString(SaveSection, DeviceIdKey, SavedDeviceId, GGameUserSettingsIni);
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

		Listener->RemoveFromRoot();
		Listener = nullptr;

		URavensightSettings* Settings = GetMutableDefault<URavensightSettings>();
		Settings->bAutoInitialize = bSavedAutoInitialize;
		Settings->bEnabled = bSavedEnabled;
		Settings->bTrackLifecycleEvents = bSavedTrackLifecycleEvents;
		Settings->FlushIntervalSeconds = SavedFlushIntervalSeconds;

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
}

#endif
