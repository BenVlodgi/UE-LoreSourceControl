// Copyright 2026 Dream Seed LLC. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LoreSourceControlVersion.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "LoreSourceControlUtils.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Misc/TextFilter.h"

class SSearchBox;
class ITableRow;
class STableViewBase;
class SWidget;
template <typename OptionType> class SComboBox;

using FLoreRevisionItemRef = TSharedRef<FLoreRevisionListEntry>;
using FLoreRevisionItemPtr = TSharedPtr<FLoreRevisionListEntry>;
using FLoreRevisionFileItemRef = TSharedRef<FLoreRevisionDeltaEntry>;

/** The repository-wide read-only history browser: a paged revision list above, the files of the selected revision below, with a branch selector. Reuses the engine's per-file history window for the file drill-down. */
class SLoreSourceControlHistoryWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreSourceControlHistoryWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	TSharedRef<SWidget> CreateToolBar();
	TSharedRef<SWidget> CreateRevisionListView();
	TSharedRef<SWidget> CreateFilesListView();

	TSharedRef<ITableRow> OnGenerateRevisionRow(FLoreRevisionItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> OnGenerateFileRow(FLoreRevisionFileItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedPtr<SWidget> OnOpenRevisionContextMenu();
	TSharedPtr<SWidget> OnOpenFileContextMenu();

	void RequestRevisionsRefresh(bool bAppend);
	void OnGetRevisionsComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, bool bAppend);
	void OnRevisionSelectionChanged(FLoreRevisionItemPtr InItem, ESelectInfo::Type SelectInfo);
	void RequestRevisionFiles(const FLoreRevisionItemPtr& InRevision);
	void OnGetRevisionFilesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, FString ForRevision);

	void OnRevisionSearchTextChanged(const FText& InFilterText);
	void PopulateRevisionSearchStrings(const FLoreRevisionListEntry& InItem, TArray<FString>& OutStrings) const;
	void RefreshRevisionRows();

	void RequestBranchList();
	void OnGetBranchesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
	TSharedRef<SWidget> OnGenerateBranchOption(TSharedPtr<FString> InOption);
	void OnBranchSelectionChanged(TSharedPtr<FString> InOption, ESelectInfo::Type SelectInfo);

	void RefreshFilesRows();

	TSharedPtr<SSearchBox> RevisionSearchBox;
	TSharedPtr<SListView<FLoreRevisionItemRef>> RevisionListView;
	TSharedPtr<SListView<FLoreRevisionFileItemRef>> FilesListView;
	TSharedPtr<TTextFilter<const FLoreRevisionListEntry&>> RevisionSearchFilter;

	TSharedPtr<SComboBox<TSharedPtr<FString>>> BranchCombo;
	TArray<TSharedPtr<FString>> BranchOptions;
	TSharedPtr<FString> SelectedBranch;

	// The revisions read so far, and the filtered subset the list renders.
	TArray<FLoreRevisionItemRef> SourceRevisions;
	TArray<FLoreRevisionItemRef> RevisionRows;

	// The delta of the currently selected revision.
	TArray<FLoreRevisionFileItemRef> FileRows;
	FString SelectedRevisionHash;

	bool bShouldRefresh = false;
	bool bIsRefreshing = false;
	bool bSourceControlAvailable = false;

	// True once a page returned a first revision (all-zeros parent), so Load more hides.
	bool bReachedFirstRevision = false;
};

#endif
