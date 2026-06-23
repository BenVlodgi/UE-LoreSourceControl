// Copyright 2026 Dream Seed LLC. MIT License.

using System.IO;
using UnrealBuildTool;

// Native Lore library (liblore), the C API. Prototype for the 0.4 SDK migration.
public class LoreLib : ModuleRules
{
	public LoreLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string LibDir = Path.Combine(ModuleDirectory, "lib", "Win64");
			PublicAdditionalLibraries.Add(Path.Combine(LibDir, "lore.lib"));
			// The windows crate's bundled import library, referenced by the Rust static library.
			PublicAdditionalLibraries.Add(Path.Combine(LibDir, "windows.0.53.0.lib"));
			// System libraries liblore reports through `rustc --print native-static-libs`, plus ntdll
			// for the NT named-pipe call Rust's std references directly (not in the reported list).
			PublicSystemLibraries.AddRange(new string[] { "kernel32.lib", "advapi32.lib", "bcrypt.lib", "legacy_stdio_definitions.lib", "ntdll.lib" });
		}
	}
}
