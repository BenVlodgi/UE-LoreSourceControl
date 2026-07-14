// Copyright 2026 Dream Seed LLC. MIT License.

#include "SLoreSourceControlBranchesWidget.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "LoreSourceControlModule.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/ComparisonUtility.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "FileHelpers.h"
#include "PackageTools.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "LoreSourceControlBranches"

// Column identifiers, one namespace per column so a header and a row agree on the same FName.
namespace LoreBranchesColumn
{
	static const FName Name("Name");
	static const FName Category("Category");
	static const FName Creator("Creator");
	static const FName Created("Created");
	static const FName Location("Location");
	static const FName Latest("Latest");
}

namespace
{
	// The location text folds the has-local and has-remote flags into one label, with a divergence marker when the two latest hashes differ.
	FText BranchLocationText(const FLoreBranchItem& InItem)
	{
		if (InItem.bHasLocal && InItem.bHasRemote)
		{
			const bool bDiverged = !InItem.LatestLocal.IsEmpty() && !InItem.LatestRemote.IsEmpty() && InItem.LatestLocal != InItem.LatestRemote;
			return bDiverged
				? LOCTEXT("LocationBothDiverged", "local and remote (diverged)")
				: LOCTEXT("LocationBoth", "local and remote");
		}
		if (InItem.bHasRemote)
		{
			return LOCTEXT("LocationRemote", "remote");
		}
		return LOCTEXT("LocationLocal", "local");
	}

	// The Latest column shows the short hash; an archived branch has an all-zeros latest, so it renders blank.
	FString ShortHash(const FString& InFull)
	{
		return InFull.Left(8);
	}
}

/** One branch row, bold when it is the current branch, per the Plastic convention. */
class SLoreSourceControlBranchRow : public SMultiColumnTableRow<FLoreBranchItemRef>
{
public:
	SLATE_BEGIN_ARGS(SLoreSourceControlBranchRow) {}
		SLATE_ARGUMENT(FLoreBranchItemPtr, Item)
		SLATE_ATTRIBUTE(FText, HighlightText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		Item = InArgs._Item;
		HighlightText = InArgs._HighlightText;
		SMultiColumnTableRow<FLoreBranchItemRef>::Construct(FSuperRowType::FArguments().ShowSelection(true), InOwner);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnId) override
	{
		const FSlateFontInfo Font = Item->bIsCurrent
			? FAppStyle::GetFontStyle("BoldFont")
			: FAppStyle::GetFontStyle("NormalFont");

		if (InColumnId == LoreBranchesColumn::Name)
		{
			return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 1.f, 2.f, 1.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Name))
				.ToolTipText(FText::FromString(Item->Name))
				.Font(Font)
				.HighlightText(HighlightText)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.f, 1.f)
			[
				// A protection indicator; enforcement is the server rejecting a push to a protected branch.
				SNew(STextBlock)
				.Text(LOCTEXT("ProtectedMark", "[protected]"))
				.ToolTipText(LOCTEXT("ProtectedTooltip", "This branch is protected. The server rejects a push to it."))
				.Font(Font)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility(Item->bProtected ? EVisibility::Visible : EVisibility::Collapsed)
			];
		}
		if (InColumnId == LoreBranchesColumn::Category)
		{
			return TextCell(FText::FromString(Item->Category), Font);
		}
		if (InColumnId == LoreBranchesColumn::Creator)
		{
			return TextCell(FText::FromString(Item->Creator), Font);
		}
		if (InColumnId == LoreBranchesColumn::Created)
		{
			const FText Date = Item->Created == FDateTime::MinValue() ? FText::GetEmpty() : FText::AsDateTime(Item->Created);
			return TextCell(Date, Font);
		}
		if (InColumnId == LoreBranchesColumn::Location)
		{
			return TextCell(BranchLocationText(*Item), Font);
		}
		if (InColumnId == LoreBranchesColumn::Latest)
		{
			// An archived branch has an all-zeros latest already normalized to empty, so the cell reads blank.
			const FString Full = !Item->LatestLocal.IsEmpty() ? Item->LatestLocal : Item->LatestRemote;
			return SNew(STextBlock)
				.Text(FText::FromString(ShortHash(Full)))
				.ToolTipText(FText::FromString(Full))
				.Margin(FMargin(6.f, 1.f))
				.Font(FAppStyle::GetFontStyle("MonospacedText"));
		}
		return SNullWidget::NullWidget;
	}

private:
	static TSharedRef<SWidget> TextCell(const FText& InText, const FSlateFontInfo& InFont)
	{
		return SNew(STextBlock)
			.Text(InText)
			.ToolTipText(InText)
			.Margin(FMargin(6.f, 1.f))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Font(InFont);
	}

	FLoreBranchItemPtr Item;
	TAttribute<FText> HighlightText;
};

void SLoreSourceControlBranchesWidget::Construct(const FArguments& InArgs)
{
	SearchTextFilter = MakeShared<TTextFilter<const FLoreBranchItem&>>(
		TTextFilter<const FLoreBranchItem&>::FItemToStringArray::CreateSP(this, &SLoreSourceControlBranchesWidget::PopulateItemSearchStrings));
	SearchTextFilter->OnChanged().AddSP(this, &SLoreSourceControlBranchesWidget::RefreshRows);

	PrimarySortColumn = LoreBranchesColumn::Name;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					CreateToolBar()
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bIncludeArchived ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						bIncludeArchived = (NewState == ECheckBoxState::Checked);
						bShouldRefresh = true;
					})
					.ToolTipText(LOCTEXT("IncludeArchivedTooltip", "Show archived branches. Lore 0.8.3 has no unarchive, so this is a viewing aid, not a restore path."))
					[
						SNew(STextBlock).Text(LOCTEXT("IncludeArchived", "Include archived"))
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.f, 0.f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchBranches", "Search branches"))
					.ToolTipText(LOCTEXT("SearchBranchesTooltip", "Filter the branches by name, creator, or category."))
					.OnTextChanged(this, &SLoreSourceControlBranchesWidget::OnSearchTextChanged)
				]
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CreateContentPanel()
		]
	];

	bShouldRefresh = true;
}

TSharedRef<SWidget> SLoreSourceControlBranchesWidget::CreateToolBar()
{
	FSlimHorizontalToolBarBuilder ToolBarBuilder(nullptr, FMultiBoxCustomization::None);
	ToolBarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateLambda([this]() { bShouldRefresh = true; })),
		NAME_None,
		LOCTEXT("RefreshButton", "Refresh"),
		LOCTEXT("RefreshButtonTooltip", "Re-read the branches from Lore."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Refresh"));
	return ToolBarBuilder.MakeWidget();
}

TSharedRef<SWidget> SLoreSourceControlBranchesWidget::CreateContentPanel()
{
	ListView = SNew(SListView<FLoreBranchItemRef>)
		.ListItemsSource(&BranchRows)
		.OnGenerateRow(this, &SLoreSourceControlBranchesWidget::OnGenerateRow)
		.OnContextMenuOpening(this, &SLoreSourceControlBranchesWidget::OnOpenContextMenu)
		.SelectionMode(ESelectionMode::Multi)
		.HeaderRow
		(
			SNew(SHeaderRow)

			+ SHeaderRow::Column(LoreBranchesColumn::Name)
			.DefaultLabel(LOCTEXT("ColName", "Name"))
			.FillWidth(2.0f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Name)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Name)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)

			+ SHeaderRow::Column(LoreBranchesColumn::Category)
			.DefaultLabel(LOCTEXT("ColCategory", "Category"))
			.FillWidth(1.0f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Category)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Category)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)

			+ SHeaderRow::Column(LoreBranchesColumn::Creator)
			.DefaultLabel(LOCTEXT("ColCreator", "Creator"))
			.FillWidth(1.5f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Creator)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Creator)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)

			+ SHeaderRow::Column(LoreBranchesColumn::Created)
			.DefaultLabel(LOCTEXT("ColCreated", "Created"))
			.FillWidth(1.5f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Created)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Created)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)

			+ SHeaderRow::Column(LoreBranchesColumn::Location)
			.DefaultLabel(LOCTEXT("ColLocation", "Location"))
			.FillWidth(1.5f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Location)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Location)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)

			+ SHeaderRow::Column(LoreBranchesColumn::Latest)
			.DefaultLabel(LOCTEXT("ColLatest", "Latest"))
			.FillWidth(1.0f)
			.SortPriority(this, &SLoreSourceControlBranchesWidget::GetColumnSortPriority, LoreBranchesColumn::Latest)
			.SortMode(this, &SLoreSourceControlBranchesWidget::GetColumnSortMode, LoreBranchesColumn::Latest)
			.OnSort(this, &SLoreSourceControlBranchesWidget::OnColumnSortModeChanged)
		);

	return ListView.ToSharedRef();
}

TSharedRef<ITableRow> SLoreSourceControlBranchesWidget::OnGenerateRow(FLoreBranchItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SLoreSourceControlBranchRow, OwnerTable)
		.Item(InItem)
		.HighlightText_Lambda([this]() { return SearchBox.IsValid() ? SearchBox->GetText() : FText::GetEmpty(); });
}

TSharedPtr<SWidget> SLoreSourceControlBranchesWidget::OnOpenContextMenu()
{
	const TArray<FString> Selected = GetSelectedBranchNames();
	if (Selected.Num() == 0)
	{
		return nullptr;
	}
	const bool bSingleSelection = Selected.Num() == 1;

	UToolMenus* ToolMenus = UToolMenus::Get();
	static const FName MenuName = "LoreSourceControl.BranchesContextMenu";
	if (!ToolMenus->IsMenuRegistered(MenuName))
	{
		ToolMenus->RegisterMenu(MenuName)->AddSection("BranchActions");
	}

	FToolMenuContext Context;
	UToolMenu* Menu = ToolMenus->GenerateMenu(MenuName, Context);
	FToolMenuSection& Section = *Menu->FindSection("BranchActions");

	const FLoreSourceControlProvider& Provider = FLoreSourceControlModule::Get().GetProvider();
	const FString SelectedBranch = Selected[0];

	// Each action's worker registered with the guarded-operation chunk, so IsOperationRegistered now reads true and the entries enable. Every entry needs a single selection, since it acts on one named branch.
	auto AddAction = [&Section, &Provider, bSingleSelection](const FName& InName, const FName& InOperationName, const FText& InLabel, const FText& InTooltip, FExecuteAction InExecute)
	{
		const bool bRegistered = Provider.IsOperationRegistered(InOperationName);
		const FText Tooltip = bRegistered
			? InTooltip
			: LOCTEXT("ActionComesLater", "This action arrives later in 0.4, with the crash protections around a working-tree rewrite.");
		Section.AddMenuEntry(
			InName,
			InLabel,
			Tooltip,
			FSlateIcon(),
			FUIAction(
				MoveTemp(InExecute),
				FCanExecuteAction::CreateLambda([bRegistered, bSingleSelection]() { return bRegistered && bSingleSelection; })));
	};

	AddAction("SwitchBranch", "SwitchBranch", LOCTEXT("SwitchBranch", "Switch workspace to this branch"), LOCTEXT("SwitchBranchTooltip", "Switch the working tree to this branch."),
		FExecuteAction::CreateSP(this, &SLoreSourceControlBranchesWidget::OnSwitchBranch, SelectedBranch));
	AddAction("CreateBranch", "CreateBranch", LOCTEXT("CreateBranch", "Create branch from this branch..."), LOCTEXT("CreateBranchTooltip", "Create a branch at the current revision and switch to it."),
		FExecuteAction::CreateSP(this, &SLoreSourceControlBranchesWidget::OnCreateBranch, SelectedBranch));
	Section.AddSeparator("LoreBranchSep1");
	AddAction("ArchiveBranch", "ArchiveBranch", LOCTEXT("ArchiveBranch", "Archive branch"), LOCTEXT("ArchiveBranchTooltip", "Archive this branch."),
		FExecuteAction::CreateSP(this, &SLoreSourceControlBranchesWidget::OnArchiveBranch, SelectedBranch));
	AddAction("ProtectBranch", "ProtectBranch", LOCTEXT("ProtectBranch", "Protect branch"), LOCTEXT("ProtectBranchTooltip", "Protect this branch so the server rejects a push to it."),
		FExecuteAction::CreateSP(this, &SLoreSourceControlBranchesWidget::OnSetProtect, SelectedBranch, true));
	AddAction("UnprotectBranch", "UnprotectBranch", LOCTEXT("UnprotectBranch", "Unprotect branch"), LOCTEXT("UnprotectBranchTooltip", "Remove this branch's protection."),
		FExecuteAction::CreateSP(this, &SLoreSourceControlBranchesWidget::OnSetProtect, SelectedBranch, false));

	return ToolMenus->GenerateWidget(Menu);
}

void SLoreSourceControlBranchesWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		bSourceControlAvailable = false;
		return;
	}

	// Refresh once when the provider becomes available.
	if (!bSourceControlAvailable)
	{
		bSourceControlAvailable = true;
		bShouldRefresh = true;
	}

	if (bShouldRefresh && !bIsRefreshing)
	{
		RequestRefresh();
		bShouldRefresh = false;
	}
}

void SLoreSourceControlBranchesWidget::RequestRefresh()
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	bIsRefreshing = true;
	TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreGetBranches>();
	Operation->bIncludeArchived = bIncludeArchived;

	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlBranchesWidget::OnGetBranchesComplete));
}

void SLoreSourceControlBranchesWidget::OnGetBranchesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	bIsRefreshing = false;
	TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetBranches>(InOperation);

	SourceItems.Reset();
	for (const FLoreBranchItem& Item : Operation->Branches)
	{
		// A stash branch is plumbing, not a user branch, so it is kept out of the branches view.
		if (Item.Category == TEXT("stash"))
		{
			continue;
		}
		SourceItems.Add(MakeShared<FLoreBranchItem>(Item));
	}

	RefreshRows();
}

void SLoreSourceControlBranchesWidget::OnSearchTextChanged(const FText& InFilterText)
{
	SearchTextFilter->SetRawFilterText(InFilterText);
	if (SearchBox.IsValid())
	{
		SearchBox->SetError(SearchTextFilter->GetFilterErrorText());
	}
}

void SLoreSourceControlBranchesWidget::PopulateItemSearchStrings(const FLoreBranchItem& InItem, TArray<FString>& OutStrings) const
{
	OutStrings.Add(InItem.Name);
	OutStrings.Add(InItem.Creator);
	OutStrings.Add(InItem.Category);
}

void SLoreSourceControlBranchesWidget::RefreshRows()
{
	BranchRows.Reset(SourceItems.Num());
	for (const FLoreBranchItemRef& Item : SourceItems)
	{
		if (SearchTextFilter->PassesFilter(Item.Get()))
		{
			BranchRows.Add(Item);
		}
	}

	SortRows();
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

EColumnSortMode::Type SLoreSourceControlBranchesWidget::GetColumnSortMode(const FName InColumnId) const
{
	if (InColumnId == PrimarySortColumn) { return PrimarySortMode; }
	if (InColumnId == SecondarySortColumn) { return SecondarySortMode; }
	return EColumnSortMode::None;
}

EColumnSortPriority::Type SLoreSourceControlBranchesWidget::GetColumnSortPriority(const FName InColumnId) const
{
	if (InColumnId == PrimarySortColumn) { return EColumnSortPriority::Primary; }
	if (InColumnId == SecondarySortColumn) { return EColumnSortPriority::Secondary; }
	return EColumnSortPriority::Max;
}

void SLoreSourceControlBranchesWidget::OnColumnSortModeChanged(const EColumnSortPriority::Type InSortPriority, const FName& InColumnId, const EColumnSortMode::Type InSortMode)
{
	if (InSortPriority == EColumnSortPriority::Primary)
	{
		PrimarySortColumn = InColumnId;
		PrimarySortMode = InSortMode;
		if (InColumnId == SecondarySortColumn)
		{
			SecondarySortColumn = FName();
			SecondarySortMode = EColumnSortMode::None;
		}
	}
	else if (InSortPriority == EColumnSortPriority::Secondary)
	{
		SecondarySortColumn = InColumnId;
		SecondarySortMode = InSortMode;
	}

	SortRows();
	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

// CompareNaturalOrder arrived at 5.3; 5.1 and 5.2 have the equivalent CompareWithNumericSuffix.
static int32 LoreCompareNaturalOrder(FStringView A, FStringView B)
{
#if LORE_UE5_3_OR_LATER
	return UE::ComparisonUtility::CompareNaturalOrder(A, B);
#else
	return UE::ComparisonUtility::CompareWithNumericSuffix(A, B);
#endif
}

void SLoreSourceControlBranchesWidget::SortRows()
{
	if (PrimarySortColumn.IsNone() || BranchRows.Num() == 0)
	{
		return;
	}

	auto Compare = [](const FName Column, const FLoreBranchItem& Lhs, const FLoreBranchItem& Rhs) -> int32
	{
		if (Column == LoreBranchesColumn::Name) { return LoreCompareNaturalOrder(*Lhs.Name, *Rhs.Name); }
		if (Column == LoreBranchesColumn::Category) { return LoreCompareNaturalOrder(*Lhs.Category, *Rhs.Category); }
		if (Column == LoreBranchesColumn::Creator) { return LoreCompareNaturalOrder(*Lhs.Creator, *Rhs.Creator); }
		if (Column == LoreBranchesColumn::Created) { return Lhs.Created < Rhs.Created ? -1 : (Lhs.Created > Rhs.Created ? 1 : 0); }
		if (Column == LoreBranchesColumn::Location) { return LoreCompareNaturalOrder(*BranchLocationText(Lhs).ToString(), *BranchLocationText(Rhs).ToString()); }
		if (Column == LoreBranchesColumn::Latest) { return LoreCompareNaturalOrder(*Lhs.LatestLocal, *Rhs.LatestLocal); }
		return 0;
	};

	const FName Primary = PrimarySortColumn;
	const FName Secondary = SecondarySortColumn;
	const EColumnSortMode::Type PrimaryMode = PrimarySortMode;
	const EColumnSortMode::Type SecondaryMode = SecondarySortMode;

	BranchRows.Sort([&Compare, Primary, Secondary, PrimaryMode, SecondaryMode](const FLoreBranchItemRef& LhsRef, const FLoreBranchItemRef& RhsRef)
	{
		const FLoreBranchItem& Lhs = LhsRef.Get();
		const FLoreBranchItem& Rhs = RhsRef.Get();
		int32 Result = Compare(Primary, Lhs, Rhs);
		if (PrimaryMode == EColumnSortMode::Descending) { Result = -Result; }
		if (Result != 0 || Secondary.IsNone()) { return Result < 0; }
		int32 SecondaryResult = Compare(Secondary, Lhs, Rhs);
		if (SecondaryMode == EColumnSortMode::Descending) { SecondaryResult = -SecondaryResult; }
		return SecondaryResult < 0;
	});
}

TArray<FString> SLoreSourceControlBranchesWidget::GetSelectedBranchNames() const
{
	TArray<FString> Names;
	if (ListView.IsValid())
	{
		for (const FLoreBranchItemRef& Item : ListView->GetSelectedItems())
		{
			Names.Add(Item->Name);
		}
	}
	return Names;
}

void SLoreSourceControlBranchesWidget::OnSwitchBranch(FString InBranchName)
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	const FText Question = FText::Format(LOCTEXT("SwitchBranchDialog", "Switch the working tree to branch {0}?"), FText::FromString(InBranchName));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Question, LOCTEXT("SwitchBranchTitle", "Switch Branch?")) != EAppReturnType::Yes)
	{
		return;
	}

	// No engine save prompt runs before a plugin-initiated switch, so the plugin saves dirty packages first; the delta packages are reloaded in the completion callback after the rewrite lands, the Unity Version Control provider's sequence.
	FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave*/ true, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true);

	bIsRefreshing = true;
	TSharedRef<FLoreSwitchBranch, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreSwitchBranch>();
	Operation->BranchName = InBranchName;
	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlBranchesWidget::OnSwitchBranchComplete));
}

void SLoreSourceControlBranchesWidget::OnSwitchBranchComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	// Reload the loaded packages the switch rewrote, so the content browser reflects the new branch. UpdatedFiles are repository-relative; only currently loaded packages need reloading.
	const TSharedRef<FLoreSwitchBranch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreSwitchBranch>(InOperation);
	const FString Root = FLoreSourceControlModule::Get().GetProvider().GetPathToRepositoryRoot();
	TArray<UPackage*> Packages;
	for (const FString& Relative : Operation->UpdatedFiles)
	{
		const FString Absolute = FPaths::ConvertRelativePathToFull(Root / Relative);
		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(Absolute, PackageName))
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Packages.Add(Package);
			}
		}
	}
	if (Packages.Num() > 0)
	{
		UPackageTools::ReloadPackages(Packages);
	}
	bIsRefreshing = false;
	bShouldRefresh = true;
}

void SLoreSourceControlBranchesWidget::OnCreateBranch(FString InParentBranchName)
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	// A small modal to name the branch. Create always switches to the new branch, but it starts at the current revision, so no packages are rewritten and no reload is needed.
	TSharedPtr<SEditableTextBox> NameEntry;
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("CreateBranchWindowTitle", "Create Branch"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	bool bAccepted = false;
	FString NewName;

	Window->SetContent(
		SNew(SBox)
		.Padding(12.0f)
		.MinDesiredWidth(320.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).Text(LOCTEXT("CreateBranchPrompt", "New branch name:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SAssignNew(NameEntry, SEditableTextBox)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CreateBranchOk", "Create"))
					.OnClicked_Lambda([&Window, &NameEntry, &NewName, &bAccepted]()
					{
						NewName = NameEntry->GetText().ToString().TrimStartAndEnd();
						bAccepted = !NewName.IsEmpty();
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CreateBranchCancel", "Cancel"))
					.OnClicked_Lambda([&Window]()
					{
						Window->RequestDestroyWindow();
						return FReply::Handled();
					})
				]
			]
		]);

	FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
	if (!bAccepted)
	{
		return;
	}

	bIsRefreshing = true;
	TSharedRef<FLoreCreateBranch, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreCreateBranch>();
	Operation->BranchName = NewName;
	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlBranchesWidget::OnBranchWriteComplete));
}

void SLoreSourceControlBranchesWidget::OnArchiveBranch(FString InBranchName)
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	const FText Question = FText::Format(LOCTEXT("ArchiveBranchDialog", "Archive branch {0}? There is no unarchive."), FText::FromString(InBranchName));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Question, LOCTEXT("ArchiveBranchTitle", "Archive Branch?")) != EAppReturnType::Yes)
	{
		return;
	}

	bIsRefreshing = true;
	TSharedRef<FLoreArchiveBranch, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreArchiveBranch>();
	Operation->BranchName = InBranchName;
	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlBranchesWidget::OnBranchWriteComplete));
}

void SLoreSourceControlBranchesWidget::OnSetProtect(FString InBranchName, bool bProtect)
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	bIsRefreshing = true;
	FSourceControlOperationRef Operation = bProtect
		? StaticCastSharedRef<ISourceControlOperation>(ISourceControlOperation::Create<FLoreProtectBranch>())
		: StaticCastSharedRef<ISourceControlOperation>(ISourceControlOperation::Create<FLoreUnprotectBranch>());
	if (bProtect)
	{
		StaticCastSharedRef<FLoreProtectBranch>(Operation)->BranchName = InBranchName;
	}
	else
	{
		StaticCastSharedRef<FLoreUnprotectBranch>(Operation)->BranchName = InBranchName;
	}
	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlBranchesWidget::OnBranchWriteComplete));
}

void SLoreSourceControlBranchesWidget::OnBranchWriteComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	bIsRefreshing = false;
	bShouldRefresh = true;
}

#undef LOCTEXT_NAMESPACE

#endif
