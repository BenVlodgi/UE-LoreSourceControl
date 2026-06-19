// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"

/** Background download of the latest Lore client release from GitHub into the bundled location. */
namespace LoreSourceControlDownload
{
	enum class EState : uint8
	{
		Idle,
		Running,
		Succeeded,
		Failed,
		Cancelled,
	};

	/** Start the download. Ignored while one is already running. Game thread only. */
	void Start();

	/** Ask a running download to stop. */
	void Cancel();

	/** Current state. */
	EState GetState();

	/** True while a download is in progress. */
	bool IsRunning();
}
