// Copyright 2026 Reality Software Entertainment. MIT licensed.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "RavensightSettings.generated.h"

/**
 * Project Settings > Plugins > Ravensight.
 *
 * Everything here is saved to DefaultGame.ini, so a designer can ship the SDK
 * without writing a line of C++ or Blueprint. Anything set here can still be
 * overridden at runtime by calling URavensightSubsystem::Init.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Ravensight"))
class RAVENSIGHT_API URavensightSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	URavensightSettings();

	virtual FName GetCategoryName() const override;

	/** Convenience accessor for the project's configured values. Never null. */
	static const URavensightSettings* Get();

	/**
	 * Base URL of the Ravensight API. "/api/v1" is appended when you leave it
	 * off, so both "https://api.ravensight.io" and the full versioned base
	 * work. Change this only if you self host.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connection")
	FString ApiUrl;

	/**
	 * Your publishable ingest key, in the form gt_live_... It is safe to ship
	 * inside a packaged build: it can only open sessions and read the tracking
	 * kill switch. It cannot read analytics, read feedback or touch your
	 * account. Rotate it from the dashboard at any time.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connection")
	FString IngestKey;

	/** Reported to the server as the client's game version. Empty uses the project version. */
	UPROPERTY(config, EditAnywhere, Category = "Connection")
	FString GameVersion;

	/**
	 * Read the kill switch and open a session as soon as the game instance
	 * comes up. Turn this off to gate initialization behind a consent prompt,
	 * then call Init yourself.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Behavior")
	bool bAutoInitialize = true;

	/** Seconds between automatic flush attempts. 0 flushes only on demand. */
	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (ClampMin = "0.0", UIMax = "60.0"))
	float FlushIntervalSeconds = 5.0f;

	/** Cap on unsent events held in memory. Oldest are dropped first once full. */
	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (ClampMin = "1", UIMax = "5000"))
	int32 MaxQueueSize = 500;

	/** Sends game_started, game_paused, game_resumed and game_exited for you. */
	UPROPERTY(config, EditAnywhere, Category = "Behavior")
	bool bTrackLifecycleEvents = true;

	/** Per request timeout in seconds. */
	UPROPERTY(config, EditAnywhere, Category = "Behavior", meta = (ClampMin = "1.0", UIMax = "60.0"))
	float RequestTimeoutSeconds = 15.0f;

	/** Logs every session, batch and retry decision to LogRavensight at Log verbosity. */
	UPROPERTY(config, EditAnywhere, Category = "Diagnostics")
	bool bVerboseLogging = false;
};
