// Copyright 2026 Dream Seed LLC. MIT License.

#include "LoreSourceControlDownload.h"
#include "LoreSourceControlLog.h"
#include "LoreSourceControlUtils.h"
#include "LoreSourceControlModule.h"
#include "LoreSourceControlProvider.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#define LOCTEXT_NAMESPACE "LoreSourceControl.Download"

namespace
{
	FThreadSafeCounter GDownloadState((int32)LoreSourceControlDownload::EState::Idle);
	FThreadSafeBool GCancelRequested(false);

#if SOURCE_CONTROL_WITH_SLATE
	TWeakPtr<SNotificationItem> GNotification;
#endif

	void FinishOnGameThread(bool bOk, bool bCancelled, int32 ReturnCode, const FString& Version, const FString& Output)
	{
		GDownloadState.Set((int32)(bOk ? LoreSourceControlDownload::EState::Succeeded : (bCancelled ? LoreSourceControlDownload::EState::Cancelled : LoreSourceControlDownload::EState::Failed)));

		if (bOk)
		{
			UE_LOG(LogLoreSourceControl, Log, TEXT("Lore CLI %s downloaded into %s"), *Version, *LoreSourceControlUtils::GetBundledClientPath());
		}
		else if (bCancelled)
		{
			UE_LOG(LogLoreSourceControl, Log, TEXT("Lore CLI download cancelled"));
		}
		else
		{
			UE_LOG(LogLoreSourceControl, Error, TEXT("Lore CLI download failed (exit %d): %s"), ReturnCode, *Output.TrimStartAndEnd());
		}

#if SOURCE_CONTROL_WITH_SLATE
		TSharedPtr<SNotificationItem> Item = GNotification.Pin();
		if (Item.IsValid())
		{
			Item->SetText(bOk
				? FText::Format(LOCTEXT("Done", "Lore CLI {0} installed"), FText::FromString(Version))
				: (bCancelled ? LOCTEXT("Cancelled", "Lore CLI download cancelled") : LOCTEXT("Failed", "Lore CLI download failed (see the log)")));
			Item->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			Item->SetExpireDuration(5.0f);
			Item->ExpireAndFadeout();
		}
		GNotification.Reset();
#endif

		// Adopt the bundled client so the next connect uses it with no manual path entry
		if (bOk)
		{
			FLoreSourceControlModule& Module = FLoreSourceControlModule::Get();
			Module.AccessSettings().SetBinaryPath(LoreSourceControlUtils::MakeBinaryPathRelativeToPlugin(LoreSourceControlUtils::GetBundledClientPath()));
			Module.AccessSettings().SaveSettings();
			Module.GetProvider().Init();
		}
	}
}

namespace LoreSourceControlDownload
{
	EState GetState()
	{
		return (EState)GDownloadState.GetValue();
	}

	bool IsRunning()
	{
		return GetState() == EState::Running;
	}

	void Cancel()
	{
		if (IsRunning())
		{
			UE_LOG(LogLoreSourceControl, Log, TEXT("Cancelling the Lore CLI download"));
			GCancelRequested = true;
		}
	}

	void Start()
	{
		if (IsRunning())
		{
			return;
		}

#if !PLATFORM_WINDOWS
		UE_LOG(LogLoreSourceControl, Warning, TEXT("Automatic download runs on Windows; install the Lore CLI manually on this platform"));
		GDownloadState.Set((int32)EState::Failed);
		return;
#else
		const FString Destination = FPaths::GetPath(LoreSourceControlUtils::GetBundledClientPath());
		if (Destination.IsEmpty())
		{
			UE_LOG(LogLoreSourceControl, Error, TEXT("Could not resolve the plugin location for the download"));
			GDownloadState.Set((int32)EState::Failed);
			return;
		}
		IFileManager::Get().MakeDirectory(*Destination, true);

		// PowerShell is the only dependency-free way to fetch and unzip a GitHub release asset on Windows
		const FString Script = FString::Printf(
			TEXT("$ErrorActionPreference='Stop'\n")
			TEXT("$headers=@{ 'User-Agent'='LoreSourceControl' }\n")
			TEXT("Write-Output 'Querying latest Lore release'\n")
			TEXT("$release=Invoke-RestMethod -Uri 'https://api.github.com/repos/EpicGames/lore/releases/latest' -Headers $headers\n")
			TEXT("$asset=$release.assets | Where-Object { $_.name -like '*x86_64-pc-windows-msvc*.zip' } | Select-Object -First 1\n")
			TEXT("if (-not $asset) { throw 'No Windows client asset in the latest release' }\n")
			TEXT("Write-Output ('version=' + $release.tag_name)\n")
			TEXT("$zip=Join-Path $env:TEMP 'lore-client.zip'\n")
			TEXT("Write-Output ('Downloading ' + $asset.name)\n")
			TEXT("Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -Headers $headers\n")
			TEXT("Write-Output 'Extracting'\n")
			TEXT("Expand-Archive -Path $zip -DestinationPath '%s' -Force\n")
			TEXT("Write-Output 'Done'\n"),
			*Destination);

		const FString ScriptPath = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("lore-download.ps1"));
		if (!FFileHelper::SaveStringToFile(Script, *ScriptPath))
		{
			UE_LOG(LogLoreSourceControl, Error, TEXT("Could not write the download script"));
			GDownloadState.Set((int32)EState::Failed);
			return;
		}

		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

		const FString Params = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\""), *ScriptPath);
		FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Params, false, true, true, nullptr, 0, nullptr, WritePipe);
		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			UE_LOG(LogLoreSourceControl, Error, TEXT("Could not launch PowerShell for the download"));
			GDownloadState.Set((int32)EState::Failed);
			return;
		}

		GCancelRequested = false;
		GDownloadState.Set((int32)EState::Running);
		UE_LOG(LogLoreSourceControl, Log, TEXT("Downloading the latest Lore CLI into %s"), *Destination);

#if SOURCE_CONTROL_WITH_SLATE
		FNotificationInfo Info(LOCTEXT("Downloading", "Downloading the latest Lore CLI..."));
		Info.bFireAndForget = false;
		Info.bUseThrobber = true;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("CancelButton", "Cancel"),
			LOCTEXT("CancelButtonTip", "Stop the download"),
			FSimpleDelegate::CreateStatic(&LoreSourceControlDownload::Cancel),
			SNotificationItem::CS_Pending));
		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
		GNotification = Item;
#endif

		Async(EAsyncExecution::Thread, [Proc, ReadPipe, WritePipe]() mutable
		{
			FString Output;
			while (FPlatformProcess::IsProcRunning(Proc))
			{
				if (GCancelRequested)
				{
					FPlatformProcess::TerminateProc(Proc, true);
					break;
				}
				Output += FPlatformProcess::ReadPipe(ReadPipe);
				FPlatformProcess::Sleep(0.2f);
			}
			Output += FPlatformProcess::ReadPipe(ReadPipe);

			int32 ReturnCode = -1;
			FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
			const bool bCancelled = GCancelRequested;
			FPlatformProcess::CloseProc(Proc);
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

			FString Version;
			TArray<FString> Lines;
			Output.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("version=")))
				{
					Version = Line.RightChop(8).TrimStartAndEnd();
				}
			}

			const bool bOk = !bCancelled && ReturnCode == 0 && LoreSourceControlUtils::IsBundledClientPresent();
			AsyncTask(ENamedThreads::GameThread, [bOk, bCancelled, ReturnCode, Version, Output]()
			{
				FinishOnGameThread(bOk, bCancelled, ReturnCode, Version, Output);
			});
		});
#endif
	}
}

#undef LOCTEXT_NAMESPACE
