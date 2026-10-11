// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The iOS side of RavensightBackgroundTask.h. UnrealBuildTool only compiles
// files under a Private/IOS folder when it builds for iOS, and the body is
// guarded by PLATFORM_IOS as well, so every other platform ignores this file.
//
// beginBackgroundTaskWithName:expirationHandler: and endBackgroundTask: may be
// called from any thread, which matters because the subsystem runs on the
// game thread, not the main thread. The expiration handler runs on the main
// thread. No lock is held across a UIKit call.

#include "RavensightBackgroundTask.h"

#if PLATFORM_IOS

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#import <UIKit/UIKit.h>

namespace
{
	FCriticalSection TaskLock;
	UIBackgroundTaskIdentifier ActiveTask = UIBackgroundTaskInvalid;

	/** Clears ActiveTask if it is Task. True when the caller should end Task. */
	bool ReleaseTask(UIBackgroundTaskIdentifier Task)
	{
		FScopeLock Lock(&TaskLock);
		if (Task == UIBackgroundTaskInvalid || ActiveTask != Task)
		{
			return false;
		}
		ActiveTask = UIBackgroundTaskInvalid;
		return true;
	}
}

void RavensightBackgroundTask::Begin()
{
	{
		FScopeLock Lock(&TaskLock);
		if (ActiveTask != UIBackgroundTaskInvalid)
		{
			return;
		}
	}

	UIApplication* Application = [UIApplication sharedApplication];

	__block UIBackgroundTaskIdentifier Task = UIBackgroundTaskInvalid;
	Task = [Application beginBackgroundTaskWithName:@"RavensightFlush" expirationHandler:^{
		// Out of time. iOS requires the task to be ended, whether or not the
		// flush finished; the background marker still reports the exit on
		// the next launch. Only the caller that releases the task ends it, so
		// it is never ended twice.
		if (ReleaseTask(Task))
		{
			[Application endBackgroundTask:Task];
		}
	}];

	if (Task == UIBackgroundTaskInvalid)
	{
		// No background time is available. The flush still runs for as long
		// as the app keeps running.
		return;
	}

	bool bAlreadyRunning = false;
	{
		FScopeLock Lock(&TaskLock);
		if (ActiveTask == UIBackgroundTaskInvalid)
		{
			ActiveTask = Task;
		}
		else
		{
			bAlreadyRunning = true;
		}
	}

	if (bAlreadyRunning)
	{
		// Another Begin won the race. One task is enough.
		[Application endBackgroundTask:Task];
	}
}

void RavensightBackgroundTask::End()
{
	UIBackgroundTaskIdentifier Task = UIBackgroundTaskInvalid;
	{
		FScopeLock Lock(&TaskLock);
		Task = ActiveTask;
	}

	if (ReleaseTask(Task))
	{
		[[UIApplication sharedApplication] endBackgroundTask:Task];
	}
}

#endif
