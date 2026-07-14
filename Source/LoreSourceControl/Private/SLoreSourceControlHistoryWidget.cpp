// Copyright 2026 Dream Seed LLC. MIT License.

#include "SLoreSourceControlHistoryWidget.h"

#if WITH_EDITOR && LORE_UE5_1_OR_LATER

#include "LoreSourceControlModule.h"
#include "LoreSourceControlOperations.h"
#include "LoreSourceControlProvider.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlWindows.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "LoreSourceControlHistory"

namespace
{
	// A page of revisions per Load more. Local reads, so a large page is one in-process call with no server round trip.
	constexpr int32 LoreHistoryPageLength = 100;

	// An all-zeros parent hash marks the first revision, so the next page starting at it would repeat the last row.
	bool IsZeroParent(const FString& InHash)
	{
		if (InHash.IsEmpty())
		{
			return true;
		}
		for (const TCHAR C : InHash)
		{
			if (C != TEXT('0'))
			{
				return false;
			}
		}
		return true;
	}
}

namespace LoreRevisionColumn
{
	static const FName Revision("Revision");
	static const FName Author("Author");
	static const FName Date("Date");
	static const FName Message("Message");
}

namespace LoreRevisionFileColumn
{
	static const FName Action("Action");
	static const FName Name("Name");
	static const FName Path("Path");
	static const FName Size("Size");
}

/** One revision row for the top pane; the full hash and parents ride the tooltip. */
class SLoreSourceControlRevisionRow : public SMultiColumnTableRow<FLoreRevisionItemRef>
{
public:
	SLATE_BEGIN_ARGS(SLoreSourceControlRevisionRow) {}
		SLATE_ARGUMENT(FLoreRevisionItemPtr, Item)
		SLATE_ATTRIBUTE(FText, HighlightText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		Item = InArgs._Item;
		HighlightText = InArgs._HighlightText;
		SMultiColumnTableRow<FLoreRevisionItemRef>::Construct(FSuperRowType::FArguments().ShowSelection(true), InOwner);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnId) override
	{
		const FText RowTooltip = FText::Format(
			LOCTEXT("RevisionRowTooltip", "Hash: {0}\nParent: {1}\nOther parent: {2}"),
			FText::FromString(Item->Hash),
			FText::FromString(Item->ParentHash),
			FText::FromString(Item->ParentOtherHash));

		if (InColumnId == LoreRevisionColumn::Revision)
		{
			return Cell(FText::AsNumber(Item->RevisionNumber), RowTooltip, false);
		}
		if (InColumnId == LoreRevisionColumn::Author)
		{
			return Cell(FText::FromString(Item->Author), FText::FromString(Item->Author), true);
		}
		if (InColumnId == LoreRevisionColumn::Date)
		{
			const FText Date = Item->Date == FDateTime::MinValue() ? FText::GetEmpty() : FText::AsDateTime(Item->Date);
			return Cell(Date, Date, false);
		}
		if (InColumnId == LoreRevisionColumn::Message)
		{
			FString OneLine = Item->Message;
			OneLine.ReplaceCharInline(TEXT('\n'), TEXT(' '), ESearchCase::CaseSensitive);
			return SNew(STextBlock)
				.Text(FText::FromString(OneLine))
				.ToolTipText(FText::FromString(Item->Message))
				.Margin(FMargin(6.f, 1.f))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.HighlightText(HighlightText);
		}
		return SNullWidget::NullWidget;
	}

private:
	TSharedRef<SWidget> Cell(const FText& InText, const FText& InTooltip, bool bHighlight)
	{
		return SNew(STextBlock)
			.Text(InText)
			.ToolTipText(InTooltip)
			.Margin(FMargin(6.f, 1.f))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.HighlightText(bHighlight ? HighlightText : TAttribute<FText>());
	}

	FLoreRevisionItemPtr Item;
	TAttribute<FText> HighlightText;
};

/** One changed-file row for the bottom pane. */
class SLoreSourceControlRevisionFileRow : public SMultiColumnTableRow<FLoreRevisionFileItemRef>
{
public:
	SLATE_BEGIN_ARGS(SLoreSourceControlRevisionFileRow) {}
		SLATE_ARGUMENT(TSharedPtr<FLoreRevisionDeltaEntry>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
	{
		Item = InArgs._Item;
		SMultiColumnTableRow<FLoreRevisionFileItemRef>::Construct(FSuperRowType::FArguments().ShowSelection(true), InOwner);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnId) override
	{
		if (InColumnId == LoreRevisionFileColumn::Action)
		{
			return Cell(FText::FromString(ActionLabel()));
		}
		if (InColumnId == LoreRevisionFileColumn::Name)
		{
			return Cell(FText::FromString(FPaths::GetCleanFilename(Item->Path)));
		}
		if (InColumnId == LoreRevisionFileColumn::Path)
		{
			return Cell(FText::FromString(Item->Path));
		}
		if (InColumnId == LoreRevisionFileColumn::Size)
		{
			return Cell(FText::AsMemory(Item->Size));
		}
		return SNullWidget::NullWidget;
	}

private:
	// A content change reads action keep with the modify flag, since Lore has no distinct edit action.
	FString ActionLabel() const
	{
		if (Item->Action == TEXT("keep"))
		{
			return Item->bModified ? TEXT("Modified") : TEXT("Unchanged");
		}
		if (Item->Action == TEXT("add")) { return TEXT("Added"); }
		if (Item->Action == TEXT("delete")) { return TEXT("Deleted"); }
		if (Item->Action == TEXT("move")) { return TEXT("Moved"); }
		if (Item->Action == TEXT("copy")) { return TEXT("Copied"); }
		return Item->Action;
	}

	TSharedRef<SWidget> Cell(const FText& InText)
	{
		return SNew(STextBlock)
			.Text(InText)
			.ToolTipText(InText)
			.Margin(FMargin(6.f, 1.f))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}

	TSharedPtr<FLoreRevisionDeltaEntry> Item;
};

void SLoreSourceControlHistoryWidget::Construct(const FArguments& InArgs)
{
	RevisionSearchFilter = MakeShared<TTextFilter<const FLoreRevisionListEntry&>>(
		TTextFilter<const FLoreRevisionListEntry&>::FItemToStringArray::CreateSP(this, &SLoreSourceControlHistoryWidget::PopulateRevisionSearchStrings));
	RevisionSearchFilter->OnChanged().AddSP(this, &SLoreSourceControlHistoryWidget::RefreshRevisionRows);

	SelectedBranch = MakeShared<FString>();

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
					SNew(STextBlock).Text(LOCTEXT("BranchLabel", "Branch:"))
				]
				// SHorizontalBox slot MinWidth arrived at 5.5, so an SBox carries the minimum width on every supported engine.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f)
				[
					SNew(SBox).MinDesiredWidth(120.f)
					[
						SAssignNew(BranchCombo, SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&BranchOptions)
						.OnGenerateWidget(this, &SLoreSourceControlHistoryWidget::OnGenerateBranchOption)
						.OnSelectionChanged(this, &SLoreSourceControlHistoryWidget::OnBranchSelectionChanged)
						.ToolTipText(LOCTEXT("BranchComboTooltip", "Show the history of this branch. Defaults to the current branch."))
						[
							SNew(STextBlock).Text_Lambda([this]()
							{
								return SelectedBranch.IsValid() && !SelectedBranch->IsEmpty()
									? FText::FromString(*SelectedBranch)
									: LOCTEXT("CurrentBranchOption", "(current branch)");
							})
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.f, 0.f)
				[
					SAssignNew(RevisionSearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchRevisions", "Search revisions"))
					.ToolTipText(LOCTEXT("SearchRevisionsTooltip", "Filter the revisions by author or message."))
					.OnTextChanged(this, &SLoreSourceControlHistoryWidget::OnRevisionSearchTextChanged)
				]
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(EOrientation::Orient_Vertical)
			+ SSplitter::Slot().Value(0.6f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					CreateRevisionListView()
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(2.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadMore", "Load more"))
					.ToolTipText(LOCTEXT("LoadMoreTooltip", "Read the next page of older revisions."))
					.Visibility_Lambda([this]() { return bReachedFirstRevision ? EVisibility::Collapsed : EVisibility::Visible; })
					.OnClicked_Lambda([this]() { RequestRevisionsRefresh(true); return FReply::Handled(); })
				]
			]
			+ SSplitter::Slot().Value(0.4f)
			[
				CreateFilesListView()
			]
		]
	];

	bShouldRefresh = true;
}

TSharedRef<SWidget> SLoreSourceControlHistoryWidget::CreateToolBar()
{
	FSlimHorizontalToolBarBuilder ToolBarBuilder(nullptr, FMultiBoxCustomization::None);
	ToolBarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateLambda([this]() { bShouldRefresh = true; })),
		NAME_None,
		LOCTEXT("RefreshButton", "Refresh"),
		LOCTEXT("RefreshButtonTooltip", "Re-read the revisions from Lore."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Refresh"));
	return ToolBarBuilder.MakeWidget();
}

TSharedRef<SWidget> SLoreSourceControlHistoryWidget::CreateRevisionListView()
{
	RevisionListView = SNew(SListView<FLoreRevisionItemRef>)
		.ListItemsSource(&RevisionRows)
		.OnGenerateRow(this, &SLoreSourceControlHistoryWidget::OnGenerateRevisionRow)
		.OnSelectionChanged(this, &SLoreSourceControlHistoryWidget::OnRevisionSelectionChanged)
		.OnContextMenuOpening(this, &SLoreSourceControlHistoryWidget::OnOpenRevisionContextMenu)
		.SelectionMode(ESelectionMode::Single)
		.HeaderRow
		(
			SNew(SHeaderRow)

			+ SHeaderRow::Column(LoreRevisionColumn::Revision)
			.DefaultLabel(LOCTEXT("ColRevision", "Revision"))
			.FillWidth(0.6f)

			+ SHeaderRow::Column(LoreRevisionColumn::Author)
			.DefaultLabel(LOCTEXT("ColAuthor", "Author"))
			.FillWidth(1.5f)

			+ SHeaderRow::Column(LoreRevisionColumn::Date)
			.DefaultLabel(LOCTEXT("ColDate", "Date"))
			.FillWidth(1.5f)

			+ SHeaderRow::Column(LoreRevisionColumn::Message)
			.DefaultLabel(LOCTEXT("ColMessage", "Message"))
			.FillWidth(4.0f)
		);

	return RevisionListView.ToSharedRef();
}

TSharedRef<SWidget> SLoreSourceControlHistoryWidget::CreateFilesListView()
{
	FilesListView = SNew(SListView<FLoreRevisionFileItemRef>)
		.ListItemsSource(&FileRows)
		.OnGenerateRow(this, &SLoreSourceControlHistoryWidget::OnGenerateFileRow)
		.OnContextMenuOpening(this, &SLoreSourceControlHistoryWidget::OnOpenFileContextMenu)
		.SelectionMode(ESelectionMode::Multi)
		.HeaderRow
		(
			SNew(SHeaderRow)

			+ SHeaderRow::Column(LoreRevisionFileColumn::Action)
			.DefaultLabel(LOCTEXT("ColFileAction", "Action"))
			.FillWidth(0.8f)

			+ SHeaderRow::Column(LoreRevisionFileColumn::Name)
			.DefaultLabel(LOCTEXT("ColFileName", "Name"))
			.FillWidth(1.5f)

			+ SHeaderRow::Column(LoreRevisionFileColumn::Path)
			.DefaultLabel(LOCTEXT("ColFilePath", "Path"))
			.FillWidth(3.0f)

			+ SHeaderRow::Column(LoreRevisionFileColumn::Size)
			.DefaultLabel(LOCTEXT("ColFileSize", "Size"))
			.FillWidth(0.8f)
		);

	return FilesListView.ToSharedRef();
}

TSharedRef<ITableRow> SLoreSourceControlHistoryWidget::OnGenerateRevisionRow(FLoreRevisionItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SLoreSourceControlRevisionRow, OwnerTable)
		.Item(InItem)
		.HighlightText_Lambda([this]() { return RevisionSearchBox.IsValid() ? RevisionSearchBox->GetText() : FText::GetEmpty(); });
}

TSharedRef<ITableRow> SLoreSourceControlHistoryWidget::OnGenerateFileRow(FLoreRevisionFileItemRef InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SLoreSourceControlRevisionFileRow, OwnerTable).Item(InItem);
}

TSharedPtr<SWidget> SLoreSourceControlHistoryWidget::OnOpenRevisionContextMenu()
{
	if (!RevisionListView.IsValid() || RevisionListView->GetNumItemsSelected() != 1)
	{
		return nullptr;
	}
	const FLoreRevisionItemRef Selected = RevisionListView->GetSelectedItems()[0];
	const FString Hash = Selected->Hash;

	FMenuBuilder MenuBuilder(true, nullptr);
	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyRevisionHash", "Copy revision hash"),
		LOCTEXT("CopyRevisionHashTooltip", "Copy this revision's full hash to the clipboard."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Hash]() { FPlatformApplicationMisc::ClipboardCopy(*Hash); })));

	// Creating a branch at a historical revision means moving the tree to that revision first, then creating, since lore branch create only creates at the current revision. That move is the MoveToRevision operation from the revision-move chunk, so the entry stays disabled until it registers, even though CreateBranch alone has landed.
	const bool bCreateAtRevisionReady = FLoreSourceControlModule::Get().GetProvider().IsOperationRegistered("CreateBranch")
		&& FLoreSourceControlModule::Get().GetProvider().IsOperationRegistered("MoveToRevision");
	MenuBuilder.AddMenuEntry(
		LOCTEXT("CreateBranchAtRevision", "Create branch at this revision..."),
		bCreateAtRevisionReady
			? LOCTEXT("CreateBranchAtRevisionTooltip", "Create a branch at this revision and switch to it.")
			: LOCTEXT("ActionComesLater", "This action arrives later in 0.4, with the guarded move between revisions."),
		FSlateIcon(),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([bCreateAtRevisionReady]() { return bCreateAtRevisionReady; })));

	return MenuBuilder.MakeWidget();
}

TSharedPtr<SWidget> SLoreSourceControlHistoryWidget::OnOpenFileContextMenu()
{
	if (!FilesListView.IsValid() || FilesListView->GetNumItemsSelected() != 1)
	{
		return nullptr;
	}
	const FLoreRevisionFileItemRef Selected = FilesListView->GetSelectedItems()[0];
	const FString RelativePath = Selected->Path;

	const FString Root = FLoreSourceControlModule::Get().GetProvider().GetPathToRepositoryRoot();
	const FString Absolute = FPaths::ConvertRelativePathToFull(Root / RelativePath);
	FString PackageName;
	const bool bIsPackage = FPackageName::TryConvertFilenameToLongPackageName(Absolute, PackageName);

	FMenuBuilder MenuBuilder(true, nullptr);
	if (bIsPackage)
	{
		// Reuse the engine's per-file history window and its diff, rather than duplicating them.
		MenuBuilder.AddMenuEntry(
			LOCTEXT("FileHistory", "File History..."),
			LOCTEXT("FileHistoryTooltip", "Open this file's revision history in the engine's history window."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([PackageName]()
			{
				FSourceControlWindows::DisplayRevisionHistory(TArray<FString>{ PackageName });
			})));
	}
	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyFilePath", "Copy path"),
		LOCTEXT("CopyFilePathTooltip", "Copy this file's repository-relative path to the clipboard."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([RelativePath]() { FPlatformApplicationMisc::ClipboardCopy(*RelativePath); })));

	return MenuBuilder.MakeWidget();
}

void SLoreSourceControlHistoryWidget::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		bSourceControlAvailable = false;
		return;
	}

	if (!bSourceControlAvailable)
	{
		bSourceControlAvailable = true;
		bShouldRefresh = true;
	}

	if (bShouldRefresh && !bIsRefreshing)
	{
		bShouldRefresh = false;
		RequestBranchList();
		RequestRevisionsRefresh(false);
	}
}

void SLoreSourceControlHistoryWidget::RequestRevisionsRefresh(bool bAppend)
{
	if (bIsRefreshing || !ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	bIsRefreshing = true;
	TSharedRef<FLoreGetRevisions, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreGetRevisions>();
	Operation->Branch = SelectedBranch.IsValid() ? *SelectedBranch : FString();
	Operation->PageLength = LoreHistoryPageLength;
	if (bAppend && SourceRevisions.Num() > 0)
	{
		// The next page starts at the oldest loaded entry's parent, because a revision is included in its own page.
		Operation->StartRevision = SourceRevisions.Last()->ParentHash;
	}

	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlHistoryWidget::OnGetRevisionsComplete, bAppend));
}

void SLoreSourceControlHistoryWidget::OnGetRevisionsComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, bool bAppend)
{
	bIsRefreshing = false;
	TSharedRef<FLoreGetRevisions, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetRevisions>(InOperation);

	if (!bAppend)
	{
		SourceRevisions.Reset();
		bReachedFirstRevision = false;
	}

	for (const FLoreRevisionListEntry& Entry : Operation->Revisions)
	{
		SourceRevisions.Add(MakeShared<FLoreRevisionListEntry>(Entry));
		if (IsZeroParent(Entry.ParentHash))
		{
			bReachedFirstRevision = true;
		}
	}
	// A short page also means the history is exhausted.
	if (Operation->Revisions.Num() < LoreHistoryPageLength)
	{
		bReachedFirstRevision = true;
	}

	RefreshRevisionRows();
}

void SLoreSourceControlHistoryWidget::OnRevisionSelectionChanged(FLoreRevisionItemPtr InItem, ESelectInfo::Type SelectInfo)
{
	if (InItem.IsValid())
	{
		RequestRevisionFiles(InItem);
	}
	else
	{
		FileRows.Reset();
		SelectedRevisionHash.Reset();
		RefreshFilesRows();
	}
}

void SLoreSourceControlHistoryWidget::RequestRevisionFiles(const FLoreRevisionItemPtr& InRevision)
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}

	SelectedRevisionHash = InRevision->Hash;
	TSharedRef<FLoreGetRevisionFiles, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreGetRevisionFiles>();
	Operation->Revision = InRevision->Hash;

	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlHistoryWidget::OnGetRevisionFilesComplete, InRevision->Hash));
}

void SLoreSourceControlHistoryWidget::OnGetRevisionFilesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, FString ForRevision)
{
	// A stale files read arriving after a newer selection is dropped.
	if (ForRevision != SelectedRevisionHash)
	{
		return;
	}
	TSharedRef<FLoreGetRevisionFiles, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetRevisionFiles>(InOperation);

	FileRows.Reset();
	for (const FLoreRevisionDeltaEntry& Delta : Operation->Details.Deltas)
	{
		// The delta lists directories too; the files pane shows only file entries.
		if (Delta.bIsFile)
		{
			FileRows.Add(MakeShared<FLoreRevisionDeltaEntry>(Delta));
		}
	}
	RefreshFilesRows();
}

void SLoreSourceControlHistoryWidget::OnRevisionSearchTextChanged(const FText& InFilterText)
{
	RevisionSearchFilter->SetRawFilterText(InFilterText);
	if (RevisionSearchBox.IsValid())
	{
		RevisionSearchBox->SetError(RevisionSearchFilter->GetFilterErrorText());
	}
}

void SLoreSourceControlHistoryWidget::PopulateRevisionSearchStrings(const FLoreRevisionListEntry& InItem, TArray<FString>& OutStrings) const
{
	OutStrings.Add(InItem.Author);
	OutStrings.Add(InItem.Message);
}

void SLoreSourceControlHistoryWidget::RefreshRevisionRows()
{
	RevisionRows.Reset(SourceRevisions.Num());
	for (const FLoreRevisionItemRef& Item : SourceRevisions)
	{
		if (RevisionSearchFilter->PassesFilter(Item.Get()))
		{
			RevisionRows.Add(Item);
		}
	}
	if (RevisionListView.IsValid())
	{
		RevisionListView->RequestListRefresh();
	}
}

void SLoreSourceControlHistoryWidget::RequestBranchList()
{
	if (!ISourceControlModule::Get().IsEnabled() || !ISourceControlModule::Get().GetProvider().IsAvailable())
	{
		return;
	}
	TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FLoreGetBranches>();
	FLoreSourceControlModule::Get().GetProvider().Execute(Operation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateSP(this, &SLoreSourceControlHistoryWidget::OnGetBranchesComplete));
}

void SLoreSourceControlHistoryWidget::OnGetBranchesComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	TSharedRef<FLoreGetBranches, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FLoreGetBranches>(InOperation);

	BranchOptions.Reset();
	// An empty option reads as the current branch.
	BranchOptions.Add(MakeShared<FString>());
	for (const FLoreBranchItem& Item : Operation->Branches)
	{
		if (Item.Category == TEXT("stash"))
		{
			continue;
		}
		BranchOptions.Add(MakeShared<FString>(Item.Name));
	}
	if (BranchCombo.IsValid())
	{
		BranchCombo->RefreshOptions();
	}
}

TSharedRef<SWidget> SLoreSourceControlHistoryWidget::OnGenerateBranchOption(TSharedPtr<FString> InOption)
{
	const FText Label = (InOption.IsValid() && !InOption->IsEmpty())
		? FText::FromString(*InOption)
		: LOCTEXT("CurrentBranchOption", "(current branch)");
	return SNew(STextBlock).Text(Label);
}

void SLoreSourceControlHistoryWidget::OnBranchSelectionChanged(TSharedPtr<FString> InOption, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}
	SelectedBranch = InOption.IsValid() ? InOption : MakeShared<FString>();
	SourceRevisions.Reset();
	RevisionRows.Reset();
	FileRows.Reset();
	SelectedRevisionHash.Reset();
	RefreshFilesRows();
	RequestRevisionsRefresh(false);
}

void SLoreSourceControlHistoryWidget::RefreshFilesRows()
{
	if (FilesListView.IsValid())
	{
		FilesListView->RequestListRefresh();
	}
}

#undef LOCTEXT_NAMESPACE

#endif
