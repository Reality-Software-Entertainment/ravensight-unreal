// Copyright 2026 Reality Software Entertainment. MIT licensed.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Interfaces/IHttpRequest.h"
#include "RavensightQueue.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RavensightSubsystem.generated.h"

class FJsonObject;
class UWorld;

DECLARE_LOG_CATEGORY_EXTERN(LogRavensight, Log, All);

/** Fired once a session token has been issued and events can go out. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FRavensightSessionReady);

/** Fired when session creation fails outright. Reason is a server error code where one was given. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightSessionFailed, const FString&, Reason);

/** Fired once, at boot, if the server side kill switch has tracking off for this game. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightTrackingDisabled, const FString&, Source);

/** Fired after a batch is accepted by the server. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightEventsFlushed, int32, Count);

/** Fired when a batch flush fails. The SDK retries on its own, so this is informational. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightFlushFailed, const FString&, Reason);

/** Fired with the result of SubmitFeedback. Reason is empty on success. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRavensightFeedbackResult, bool, bSuccess, const FString&, Reason);

/**
 * Fired with the result of FetchSuggestions. Each entry is one suggestion
 * serialized as a JSON object string, because the shape is still evolving
 * server side and a USTRUCT would pin it prematurely.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRavensightSuggestionsReceived, bool, bSuccess, const TArray<FString>&, Suggestions);

/**
 * The Ravensight entry point.
 *
 * A game instance subsystem, so it exists for the whole run, survives level
 * loads, and is reachable from Blueprint with a single Get Ravensight
 * Subsystem node.
 *
 * With an ingest key set in Project Settings > Plugins > Ravensight it starts
 * itself: it reads the kill switch, opens a session and flushes on a timer.
 * Nothing else is required.
 */
UCLASS()
class RAVENSIGHT_API URavensightSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// --- UGameInstanceSubsystem -------------------------------------------

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// --- Access ------------------------------------------------------------

	/** Blueprint friendly accessor. Returns null outside a live game instance. */
	UFUNCTION(BlueprintPure, Category = "Ravensight", meta = (WorldContext = "WorldContextObject", DisplayName = "Get Ravensight"))
	static URavensightSubsystem* Get(const UObject* WorldContextObject);

	// --- Setup -------------------------------------------------------------

	/**
	 * Configures and starts the SDK. Only needed when you turned off
	 * Auto Initialize, for example to wait on a consent prompt, or when the
	 * key comes from somewhere other than Project Settings.
	 *
	 * Leave ApiUrl or GameVersion empty to keep the configured value.
	 * Calling this twice is a no-op after the first success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void Init(const FString& InIngestKey, const FString& InApiUrl = FString(), const FString& InGameVersion = FString());

	/**
	 * Local opt in and opt out, for a privacy toggle in your options menu.
	 * Disabling stops all sending and discards anything still queued. This is
	 * separate from the server side kill switch, which you control from the
	 * dashboard.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SetTrackingEnabled(bool bEnabled);

	// --- Tracking ----------------------------------------------------------

	/**
	 * Queues an event with string valued properties, which is what Blueprint
	 * can express directly. Safe to call before the session is ready: events
	 * are held and sent once one exists.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight", meta = (AutoCreateRefTerm = "Data"))
	void TrackEvent(const FString& EventName, const TMap<FString, FString>& Data);

	/**
	 * Queues an event whose payload is a JSON object literal, for numbers,
	 * bools, arrays or nesting. An unparseable string is sent as an empty
	 * object and logged.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void TrackEventJson(const FString& EventName, const FString& JsonData);

	/** Sends queued events now, ignoring the retry window. */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void Flush();

	// --- Feedback and suggestions -----------------------------------------

	/**
	 * Posts free form player feedback. Category and Rating are optional; pass
	 * a Rating of 1 to 5 if you collected one, or 0 to omit it. The result
	 * arrives on OnFeedbackResult.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SubmitFeedback(const FString& Message, const FString& FeedbackCategory = FString(), int32 Rating = 0);

	/**
	 * EXPERIMENTAL. Requests AI generated design suggestions for this game.
	 * Empty until the game has enough data for a weekly digest, and the shape
	 * of each entry may change, so do not build game logic on it. The result
	 * arrives on OnSuggestionsReceived.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight|Experimental")
	void FetchSuggestions();

	// --- State -------------------------------------------------------------

	/** True once a session token exists and has not expired. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	bool IsSessionReady() const;

	/** True when both the server kill switch and the local toggle allow tracking. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	bool IsTrackingActive() const;

	/** How many events are waiting to be sent. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	int32 GetQueuedEventCount() const;

	/** The generated, non advertising device identifier this install reports. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	FString GetDeviceId() const;

	// --- Events ------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightSessionReady OnSessionReady;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightSessionFailed OnSessionFailed;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightTrackingDisabled OnTrackingDisabled;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightEventsFlushed OnEventsFlushed;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightFlushFailed OnFlushFailed;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight")
	FRavensightFeedbackResult OnFeedbackResult;

	UPROPERTY(BlueprintAssignable, Category = "Ravensight|Experimental")
	FRavensightSuggestionsReceived OnSuggestionsReceived;

	// --- Escape hatch ------------------------------------------------------

	/** The protocol state machine, for tests and debug overlays. */
	const FRavensightQueue& GetQueue() const { return Queue; }

	/** Accepts a bare host or a full /api/v1 base and always returns the versioned base. */
	static FString NormalizeApiUrl(const FString& InUrl);

private:
	// --- Requests ----------------------------------------------------------

	void CheckServerSettings();
	void HandleSettingsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void AfterSettingsChecked();

	void EnsureSession();
	void HandleSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

	void FlushInternal(bool bForce);
	void SendBatch(const TArray<FRavensightEvent>& Batch);
	void HandleBatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 BatchSize);

	void HandleFeedbackResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void HandleSuggestionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

	/** Builds a request with the shared timeout and verb already applied. */
	FHttpRequestRef MakeRequest(const FString& Verb, const FString& Path) const;

	// --- Timers ------------------------------------------------------------

	void StartFlushTimer();
	void StopTimers();
	void ScheduleRetry();
	void OnFlushTimer();
	void OnRetryTimer();

	// --- Lifecycle hooks ---------------------------------------------------

	void BindApplicationDelegates();
	void UnbindApplicationDelegates();
	void OnApplicationWillDeactivate();
	void OnApplicationEnteredForeground();
	void OnWorldBeginTearDown(UWorld* World);

	// --- Helpers -----------------------------------------------------------

	static double NowSeconds();
	static FString ResolveDeviceId();
	static FString ResolveGameVersion();
	void Log(const FString& Message) const;

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Body);
	static FString ExtractError(const FString& Body, const FString& Fallback);
	static double ParseRetryAfterSeconds(const FHttpResponsePtr& Response);
	static FString ResponseBody(const FHttpResponsePtr& Response);

	// --- State -------------------------------------------------------------

	FRavensightQueue Queue;

	FString IngestKey;
	FString ApiUrl;
	FString GameVersion;
	FString Platform;
	FString DeviceId;

	bool bInitialized = false;
	bool bSettingsChecked = false;
	bool bSessionRequestPending = false;
	bool bShuttingDown = false;

	float FlushIntervalSeconds = 5.0f;
	float RequestTimeoutSeconds = 15.0f;
	bool bTrackLifecycleEvents = true;
	bool bVerboseLogging = false;

	FTimerHandle FlushTimerHandle;
	FTimerHandle RetryTimerHandle;

	FDelegateHandle DeactivateHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle TearDownHandle;
};
