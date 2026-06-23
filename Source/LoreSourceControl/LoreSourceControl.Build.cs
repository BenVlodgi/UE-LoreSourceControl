// Copyright 2026 Dream Seed LLC. MIT License.

using System.IO;
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
				"Json",
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
				// The "Check in over lock" asset menu uses ToolMenus and the Content Browser, gated to UE5.
				PrivateDependencyModuleNames.Add("ToolMenus");
				PrivateDependencyModuleNames.Add("ContentBrowser");
			}

			// The submit-dialog title override binds a SourceControlWindows delegate added in UE 5.8.
			if (Target.Version.MajorVersion > 5 || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8))
			{
				PrivateDependencyModuleNames.Add("SourceControlWindows");
			}
		}

		// liblore native library, Win64 only. The static library is gitignored and rebuilt from the pinned source, so a clean
		// clone that lacks it builds the command-line path; a tree that has it (a release package build) links the native path.
		string LoreLibrary = Path.Combine(ModuleDirectory, "..", "ThirdParty", "LoreLib", "lib", "Win64", "lore.lib");
		if (Target.Platform == UnrealTargetPlatform.Win64 && File.Exists(LoreLibrary))
		{
			PrivateDependencyModuleNames.Add("LoreLib");
			PublicDefinitions.Add("LORE_WITH_LIBLORE=1");
		}
		else
		{
			PublicDefinitions.Add("LORE_WITH_LIBLORE=0");
		}
	}
}
