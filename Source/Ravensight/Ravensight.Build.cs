// Copyright 2026 Reality Software Entertainment. MIT licensed.

using UnrealBuildTool;

public class Ravensight : ModuleRules
{
	public Ravensight(ReadOnlyTargetRules Target) : base(Target)
	{
		// Every header in this module includes what it uses, so the shared PCH
		// path is safe. bEnforceIWYU / IWYUSupport are deliberately left at
		// their defaults: they were renamed in 5.2 and setting either one
		// pins the plugin to a narrower engine range than it needs.
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
			}
		);

		if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			// Private/IOS/RavensightIOSBackgroundTask.mm wraps the background
			// flush in a UIKit background task. UnrealBuildTool compiles a
			// Private/IOS folder only for iOS, so other platforms never see it.
			PublicFrameworks.Add("UIKit");
		}
	}
}
