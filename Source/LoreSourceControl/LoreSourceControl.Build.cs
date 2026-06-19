// Copyright 2026 Dream Seed LLC. MIT License.

using UnrealBuildTool;

public class LoreSourceControl : ModuleRules
{
	public LoreSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"SourceControl",
				"Projects",
			}
		);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"CoreUObject",
					"UnrealEd",
				}
			);

			// EditorFramework arrived in UE 5.0; on 4.27 its types come from UnrealEd
			if (Target.Version.MajorVersion >= 5)
			{
				PrivateDependencyModuleNames.Add("EditorFramework");
			}
		}
	}
}
