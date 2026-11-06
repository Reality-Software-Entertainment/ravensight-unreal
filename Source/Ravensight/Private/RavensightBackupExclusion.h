// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// iOS only: marks a file as excluded from iCloud and device backups
// (NSURLIsExcludedFromBackupKey). The persisted queue holds origin stamps and
// scrubbed error captures, which have no place in a backup. Everywhere else
// the call does nothing; Android's Auto Backup is configured by the game (see
// the README). Internal to the module.

#pragma once

#include "CoreMinimal.h"

namespace RavensightBackupExclusion
{
#if PLATFORM_IOS
	/** Sets the do-not-backup attribute on the file at Path. Returns false when it could not. */
	bool Exclude(const FString& Path);
#else
	inline bool Exclude(const FString& /*Path*/) { return true; }
#endif
}
