// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "LoreSourceControlSettings.h"
#include "LoreSourceControlProvider.h"

class FLoreSourceControlModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FLoreSourceControlSettings& AccessSettings() { return Settings; }
	const FLoreSourceControlSettings& GetSettings() const { return Settings; }

	FLoreSourceControlProvider& GetProvider() { return Provider; }
	const FLoreSourceControlProvider& GetProvider() const { return Provider; }

	static FLoreSourceControlModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FLoreSourceControlModule>("LoreSourceControl");
	}

	static FLoreSourceControlModule* GetPtr()
	{
		return FModuleManager::GetModulePtr<FLoreSourceControlModule>("LoreSourceControl");
	}

private:
	FLoreSourceControlSettings Settings;
	FLoreSourceControlProvider Provider;
};
