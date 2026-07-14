// Copyright 2026 Dream Seed LLC. MIT License.

#include "LorePluginStore.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

namespace LorePluginStore
{
	FString GetStoreRoot(const FString& InRepositoryRoot)
	{
		return FPaths::Combine(InRepositoryRoot, TEXT(".lore"), TEXT(".ueplugin"));
	}

	FString GetStorePath(const FString& InRepositoryRoot, const FString& InRelativePath)
	{
		return FPaths::Combine(GetStoreRoot(InRepositoryRoot), InRelativePath);
	}

	FString ReadInstanceId(const FString& InRepositoryRoot)
	{
		const FString InstanceFile = FPaths::Combine(InRepositoryRoot, TEXT(".lore"), TEXT("instance"));
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *InstanceFile) || Bytes.Num() == 0)
		{
			return FString();
		}
		FString Hex;
		Hex.Reserve(Bytes.Num() * 2);
		for (uint8 Byte : Bytes)
		{
			Hex.Appendf(TEXT("%02x"), Byte);
		}
		return Hex;
	}

	bool ReadJsonObject(const FString& InAbsolutePath, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *InAbsolutePath))
		{
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool WriteJsonObjectAtomic(const FString& InAbsolutePath, const TSharedRef<FJsonObject>& InObject)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(InObject, Writer))
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(InAbsolutePath), true);

		// Write a unique sibling temporary file, then rename over the destination. A rename on the same volume is atomic, so a reader sees the old file or the new one, never a partial write.
		const FString TempPath = InAbsolutePath + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *TempPath))
		{
			return false;
		}
		if (!IFileManager::Get().Move(*InAbsolutePath, *TempPath, true, true))
		{
			IFileManager::Get().Delete(*TempPath, false, true);
			return false;
		}
		return true;
	}
}
