// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// iOS only: asks the system for a little extra time after the app enters the
// background, so the flush started there can finish before iOS suspends the
// process. Everywhere else both calls do nothing. Internal to the module.

#pragma once

#include "CoreMinimal.h"

namespace RavensightBackgroundTask
{
#if PLATFORM_IOS
	/**
	 * Begins a UIKit background task (beginBackgroundTaskWithName:
	 * expirationHandler:) unless one is already running. If iOS runs out of
	 * time first, the expiration handler ends the task itself.
	 */
	void Begin();

	/** Ends the running background task, if any. Safe to call more than once. */
	void End();
#else
	inline void Begin() {}
	inline void End() {}
#endif
}
