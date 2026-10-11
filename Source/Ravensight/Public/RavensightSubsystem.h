// Copyright 2026 Reality Software Entertainment. MIT licensed.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Interfaces/IHttpRequest.h"
#include "RavensightProtocol.h"
#include "RavensightQueue.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RavensightSubsystem.generated.h"

class FJsonObject;
class UWorld;

DECLARE_LOG_CATEGORY_EXTERN(LogRavensight, Log, All);

/** Fired once a session token has been issued and events can go out. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FRavensightSessionReady);

/** Fired when session creation fails. Reason is a server error code where one was given. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightSessionFailed, const FString&, Reason);

/**
 * Fired once when tracking stops for the rest of the run: "server" for the
 * kill switch (from GET /settings or a 403 tracking_disabled), or
 * "playtest_job_closed" when a Playtest run's job has ended.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightTrackingDisabled, const FString&, Source);

/**
 * Fired whenever the status reason changes. Empty means ready; otherwise one
 * of disabled, invalid_key, tracking_disabled, offline, rate_limited,
 * session_failed or playtest_job_closed.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRavensightStatusChanged, const FString&, Reason);

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
 * itself: it reads the player's saved choice, reads the kill switch, opens a
 * session and flushes on a timer. Nothing else is required.
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
	 * The player's opt in and opt out, for a privacy toggle in your options
	 * menu. The choice is saved to GameUserSettings.ini straight away and read
	 * back on every launch before anything is sent, so an opted out player's
	 * machine never contacts Ravensight. Turning it off discards anything
	 * queued and stops the timer. A saved opt out wins over the Enabled
	 * setting. Ignored, with one log line, during a Playtest run.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SetTrackingEnabled(bool bEnabled);

	/**
	 * Writes the player's choice without a running subsystem, for code that
	 * runs before the game instance exists. The subsystem reads it when it
	 * starts.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	static void SaveTrackingEnabled(bool bEnabled);

	/** The saved player choice. True when nothing has been saved. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	static bool LoadTrackingEnabled();

	/**
	 * The developer switch, the runtime side of the Enabled project setting.
	 * Not saved. While off, no request of any kind is made.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SetEnabled(bool bInEnabled);

	/**
	 * Forgets this install's analytics identity: mints and saves a new device
	 * id, clears the queue and drops the session, so the next event opens a
	 * session under the new id. Returns the new id. Ignored, with one log
	 * line, during a Playtest run.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	FString ResetDeviceId();

	/**
	 * Marks this run as a Ravensight Playtest run. Call it before Init (turn
	 * Auto Initialize off), for platforms where the runner cannot set
	 * environment variables or a command line. Desktop builds read
	 * RAVENSIGHT_PLAYTEST_TOKEN and RAVENSIGHT_PLAYTEST_RUN_ID on their own.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SetPlaytestContext(const FString& Token, const FString& RunId, const FString& JobId, const FString& Persona);

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
	 * Posts free form player feedback. Category is optional and one of
	 * GetFeedbackCategories(); pass a Rating of 1 to 5 if you collected one,
	 * or 0 to omit it. A bad category or rating fails locally with
	 * invalid_category or invalid_rating and sends nothing. The result
	 * arrives on OnFeedbackResult.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight")
	void SubmitFeedback(const FString& Message, const FString& FeedbackCategory = FString(), int32 Rating = 0);

	/** bug, suggestion, complaint, praise, playtest, other. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	static TArray<FString> GetFeedbackCategories();

	/**
	 * EXPERIMENTAL. Requests AI generated design suggestions for this game.
	 * Empty until the game has enough data for a weekly digest, and the shape
	 * of each entry may change, so do not build game logic on it. The result
	 * arrives on OnSuggestionsReceived; while tracking is off it arrives at
	 * once as a failure with an empty list and no request is made.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ravensight|Experimental")
	void FetchSuggestions();

	// --- State -------------------------------------------------------------

	/** True once a session token exists and has not expired. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	bool IsSessionReady() const;

	/** True when the Enabled setting, the server kill switch and the player's choice all allow tracking. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	bool IsTrackingActive() const;

	/**
	 * Why the SDK is not ready. Empty when ready or not started; otherwise
	 * disabled, invalid_key, tracking_disabled, offline, rate_limited,
	 * session_failed or playtest_job_closed. OnStatusChanged fires on change.
	 */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	FString GetStatusReason() const;

	/**
	 * The current session token, empty until a session opens. Hand it to
	 * your game server as joinToken so its events join this player's session.
	 */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	FString GetSessionToken() const;

	/** True when this run is a Ravensight Playtest run. */
	UFUNCTION(BlueprintPure, Category = "Ravensight")
	bool IsPlaytest() const;

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
	FRavensightStatusChanged OnStatusChanged;

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

	/** The plugin version, "0.2.1". */
	static FString GetSdkVersion() { return RavensightVersion::Sdk; }

private:
	// --- Start and stop ----------------------------------------------------

	/** Starts the timer, queues game_started once and runs the settings check, when allowed. */
	void StartIfAllowed();

	/** Stops for the rest of the run: kill switch or Playtest stop. */
	void StopForRun(const FString& Reason, const FString& Source);

	/** The ingest key was refused: automatic retries stop, the queue is kept. */
	void RefuseKey();

	void SetStatusReason(const FString& Reason);
	/** bAtFront puts it ahead of events the game tracked before Init. */
	void QueueGameStartedOnce(bool bAtFront = false);

	/** Validates, tags and queues one event. TrackEventJson and the lifecycle events share it. */
	void EnqueueEvent(const FString& EventName, const FString& JsonData, bool bAtFront);

	/** The pure start gate (RavensightProtocol::DecideGate) over the current state. */
	FRavensightGate CurrentGate(bool bExplicitAttempt = false) const;

	/** Anything already in flight belongs to an older queue and its answer is ignored. */
	void BumpQueueGeneration() { ++QueueGeneration; }
	void ReadPlaytestSources();

	// --- Requests ----------------------------------------------------------

	void CheckServerSettings();
	void HandleSettingsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void AfterSettingsChecked();

	void EnsureSession(bool bExplicit = false);
	void HandleSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 Generation);

	void FlushInternal(bool bForce, bool bExplicit = false);
	void SendBatch(const TArray<FRavensightEvent>& Batch);
	void HandleBatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int32 BatchSize, int32 Generation);

	void HandleFeedbackResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void HandleSuggestionsResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

	/** One POST /track/batch carrying only game_exited under a given session token. One attempt, no retry. */
	void SendExitUnderToken(const FString& Token, double ExitedAtSeconds);

	/** Builds a request with the shared timeout, verb and Playtest header already applied. */
	FHttpRequestRef MakeRequest(const FString& Verb, const FString& Path) const;

	/** Classifies an answer with the shared table in RavensightProtocol. */
	static FRavensightResponseVerdict Classify(ERavensightEndpoint Endpoint, const FHttpResponsePtr& Response, bool bConnectedSuccessfully, int32 BatchSize = 0, int32 InConsecutiveBatchUnauthorized = 0);

	/** Holds the quit for at most ExitFlushBudgetSeconds while a session or batch request is in flight. */
	void PumpUntilIdle(double BudgetSeconds);

	/**
	 * iOS: the background flush runs inside a UIKit background task, ended
	 * once no settings, session or batch request is in flight, on return to
	 * the foreground, or on quit. Does nothing on other platforms.
	 */
	void BeginBackgroundTask();
	void EndBackgroundTask();
	void EndBackgroundTaskIfIdle();

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
	void OnApplicationWillEnterBackground();
	void OnApplicationEnteredForeground();
	void OnWorldBeginTearDown(UWorld* World);

	/** iOS and Android: the background marker that lets a swipe killed app report its exit next launch. */
	void WriteBackgroundMarker();
	void DeleteBackgroundMarker();
	void ReportMarkerExitFromLastRun();

	// --- Helpers -----------------------------------------------------------

	static double NowSeconds();
	static FString ResolveDeviceId();
	static FString GenerateDeviceId();
	static FString ResolveGameVersion();
	static bool IsMobilePlatform();
	void Log(const FString& Message) const;

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Body);
	static FString ExtractError(const FString& Body, const FString& Fallback);
	static double ParseRetryAfterSeconds(const FHttpResponsePtr& Response);
	static FString ResponseBody(const FHttpResponsePtr& Response);

	// --- State -------------------------------------------------------------

	FRavensightQueue Queue;
	FRavensightLifecycleGuards Lifecycle;

	FString IngestKey;
	FString ApiUrl;
	FString GameVersion;
	FString Platform;
	FString DeviceId;
	FString StatusReason;

	/** Playtest context. Token empty outside a Playtest run. */
	FString PlaytestToken;
	FString PlaytestRunId;
	FString PlaytestJobId;
	FString PlaytestPersona;

	bool bInitialized = false;
	bool bStarted = false;
	bool bSettingsChecked = false;
	bool bSettingsRequestPending = false;
	bool bSessionRequestPending = false;
	bool bShuttingDown = false;

	/** Kill switch or Playtest stop. Nothing more is sent this run. */
	bool bStoppedForRun = false;

	/** The key was refused. Only an explicit Flush makes another attempt. */
	bool bKeyRefused = false;
	bool bKeyRefusedLogged = false;
	bool bDroppedEventLogged = false;
	bool bMarkerChecked = false;

	/** Shrinks after a 400 on /track/batch, back to MaxBatchSize after a 202. */
	int32 BatchLimit = RavensightLimits::MaxBatchSize;

	/** 401s on /track/batch since the last 202. */
	int32 ConsecutiveBatchUnauthorized = 0;

	/**
	 * Bumped whenever the queue or the session is thrown away (opt out,
	 * Enabled off, ResetDeviceId, stop for the run). A batch or session
	 * answer from an older generation is ignored.
	 */
	int32 QueueGeneration = 0;

	/** iOS: a background task is running for the background flush. */
	bool bBackgroundTaskActive = false;

	/** iOS and Android: when the app last went to the background, 0 when in the foreground. */
	double BackgroundedAtSeconds = 0.0;

	float FlushIntervalSeconds = 5.0f;
	float RequestTimeoutSeconds = 15.0f;
	float SessionTimeoutSeconds = 1800.0f;
	bool bTrackLifecycleEvents = true;
	bool bTrackPauseEvents = false;
	bool bVerboseLogging = false;

	FTimerHandle FlushTimerHandle;
	FTimerHandle RetryTimerHandle;

	FDelegateHandle DeactivateHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle TearDownHandle;
};
