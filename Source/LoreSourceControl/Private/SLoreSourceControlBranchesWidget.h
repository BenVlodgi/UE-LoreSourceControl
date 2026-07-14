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

using FLoreBranchItemRef = TSharedRef<FLoreBranchItem>;
using FLoreBranchItemPtr = TSharedPtr<FLoreBranchItem>;

/** The branches panel: a searchable, sortable list of the repository's branches with an archived toggle, the current branch bold, protection shown, and a context menu whose guarded write actions stay disabled until their workers register. */
class SLoreSourceControlBranchesWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLoreSourceControlBranchesWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	TSharedRef<SWidget> CreateToolBar();
	TSharedRef<SWidget> CreateContentPanel();

	TSharedRef<ITableRow> OnGenerateRow(FLoreBranchItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedPtr<SWidget> OnOpenContextMenu();

	void RequestRefresh();
	void OnGetBranchesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);

	// Guarded branch write actions, dispatched from the context menu. Each refreshes the list on completion; the switch also saves dirty packages, unlinks them before the rewrite, and reloads the delta packages after, the Unity Version Control provider's pattern.
	void OnSwitchBranch(FString InBranchName);
	void OnSwitchBranchComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
	void OnCreateBranch(FString InParentBranchName);
	void OnArchiveBranch(FString InBranchName);
	void OnSetProtect(FString InBranchName, bool bProtect);
	void OnBranchWriteComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);

	void OnSearchTextChanged(const FText& InFilterText);
	void PopulateItemSearchStrings(const FLoreBranchItem& InItem, TArray<FString>& OutStrings) const;
	void RefreshRows();

	EColumnSortMode::Type GetColumnSortMode(const FName InColumnId) const;
	EColumnSortPriority::Type GetColumnSortPriority(const FName InColumnId) const;
	void OnColumnSortModeChanged(const EColumnSortPriority::Type InSortPriority, const FName& InColumnId, const EColumnSortMode::Type InSortMode);
	void SortRows();

	TArray<FString> GetSelectedBranchNames() const;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FLoreBranchItemRef>> ListView;
	TSharedPtr<TTextFilter<const FLoreBranchItem&>> SearchTextFilter;

	// The full branch set from the last read, and the filtered subset the list view renders.
	TArray<FLoreBranchItemRef> SourceItems;
	TArray<FLoreBranchItemRef> BranchRows;

	FName PrimarySortColumn;
	FName SecondarySortColumn;
	EColumnSortMode::Type PrimarySortMode = EColumnSortMode::Ascending;
	EColumnSortMode::Type SecondarySortMode = EColumnSortMode::None;

	bool bIncludeArchived = false;
	bool bShouldRefresh = false;
	bool bIsRefreshing = false;
	bool bSourceControlAvailable = false;
};

#endif
