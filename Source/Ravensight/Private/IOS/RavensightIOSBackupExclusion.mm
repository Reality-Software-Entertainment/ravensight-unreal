// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The iOS side of RavensightBackupExclusion.h. UnrealBuildTool compiles a
// Private/IOS folder only for iOS, and the body is guarded by PLATFORM_IOS.

#include "RavensightBackupExclusion.h"

#if PLATFORM_IOS

#include "HAL/FileManager.h"

#import <Foundation/Foundation.h>

bool RavensightBackupExclusion::Exclude(const FString& Path)
{
	// The engine's relative paths become absolute file system paths first.
	const FString Absolute = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*Path);
	@autoreleasepool
	{
		NSURL* Url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Absolute)]];
		NSError* Error = nil;
		return [Url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&Error] == YES;
	}
}

#endif
