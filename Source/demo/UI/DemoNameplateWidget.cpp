#include "UI/DemoNameplateWidget.h"
#include "Core/DemoCharacterBase.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

TSharedRef<SWidget> UDemoNameplateWidget::RebuildWidget()
{
	// 先 Initialize() 让 WidgetTree 就位（蓝图子类的树在这一步才装上），再建树。
	// 详见 UDemoStatusBarWidget::RebuildWidget 的注释。
	Initialize();

	EnsureDefaultTree();
	return Super::RebuildWidget();
}

void UDemoNameplateWidget::EnsureDefaultTree()
{
	if (bTreeReady)
	{
		return;
	}

	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (!WidgetTree)
	{
		return;
	}

	// 设计师已经在 UMG 里搭好了 → 不覆盖
	if (WidgetTree->RootWidget)
	{
		bTreeReady = true;
		return;
	}

	RootSizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("NameplateSizeBox"));
	if (!RootSizeBox)
	{
		return;
	}
	RootSizeBox->SetWidthOverride(PanelSize.X);
	RootSizeBox->SetHeightOverride(PanelSize.Y);

	RootBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("NameplateBox"));
	if (!RootBox)
	{
		return;
	}
	RootSizeBox->AddChild(RootBox);

	// ---- 名字 ----
	NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NameplateName"));
	if (NameText)
	{
		FSlateFontInfo Font = NameText->GetFont();
		Font.Size = FMath::Max(NameFontSize, 6);
		NameText->SetFont(Font);
		NameText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.92f)));
		NameText->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f));
		NameText->SetShadowOffset(FVector2D(1.f, 1.f));

		if (UVerticalBoxSlot* VSlot = RootBox->AddChildToVerticalBox(NameText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
		}
	}

	// ---- 血条：SizeBox 固定高度，里面 Overlay 叠三层 ----
	USizeBox* BarSizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("NameplateBarSizeBox"));
	if (!BarSizeBox)
	{
		return;
	}
	BarSizeBox->SetHeightOverride(BarHeight);

	UOverlay* BarOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("NameplateBarOverlay"));
	if (!BarOverlay)
	{
		return;
	}
	BarSizeBox->AddChild(BarOverlay);

	auto AddFullSlot = [BarOverlay](UWidget* Child) -> void
	{
		if (!Child)
		{
			return;
		}
		BarOverlay->AddChildToOverlay(Child);
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Child->Slot))
		{
			OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
			OverlaySlot->SetVerticalAlignment(VAlign_Fill);
		}
	};

	BackBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("NameplateBack"));
	if (BackBorder)
	{
		BackBorder->SetBrushColor(BackColor);
		AddFullSlot(BackBorder);
	}

	GhostBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("NameplateGhost"));
	if (GhostBar)
	{
		GhostBar->SetPercent(1.f);
		GhostBar->SetFillColorAndOpacity(FLinearColor(0.95f, 0.86f, 0.55f, 0.85f));
		AddFullSlot(GhostBar);
	}

	FillBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("NameplateFill"));
	if (FillBar)
	{
		FillBar->SetPercent(1.f);
		FillBar->SetFillColorAndOpacity(FillColor);
		AddFullSlot(FillBar);
	}

	if (UVerticalBoxSlot* VSlot = RootBox->AddChildToVerticalBox(BarSizeBox))
	{
		VSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	WidgetTree->RootWidget = RootSizeBox;

	bTreeReady = true;

	// 先按"满血"状态摆一次
	if (FillBar)
	{
		FillBar->SetPercent(1.f);
	}
	if (GhostBar)
	{
		GhostBar->SetPercent(1.f);
	}

	ApplyVisibility();
}

// ==================================================================================
// 运行时
// ==================================================================================
void UDemoNameplateWidget::BindToCharacter(ADemoCharacterBase* InCharacter)
{
	BoundCharacter = InCharacter;

	if (!InCharacter)
	{
		return;
	}

	// 优先用角色上配的显示名（BP_Boss 的 DisplayName），没配才退回 Actor 名
	const FText DisplayName = InCharacter->DisplayName.IsEmpty()
		? FText::FromString(InCharacter->GetName())
		: InCharacter->DisplayName;
	SetNameText(DisplayName);

	// 立刻同步一次当前血量，避免"刚生成时条是满的，等下一次掉血才跳正"
	SetHealthValue(InCharacter->GetHealth(), InCharacter->GetMaxHealth());
}

void UDemoNameplateWidget::SetNameText(const FText& InName)
{
	if (NameText)
	{
		NameText->SetText(InName);
	}
}

void UDemoNameplateWidget::SetHealthValue(float NewHealth, float MaxHealth)
{
	TargetPercent = (MaxHealth > KINDA_SMALL_NUMBER) ? FMath::Clamp(NewHealth / MaxHealth, 0.f, 1.f) : 0.f;

	if (GhostPercent < TargetPercent)
	{
		GhostPercent = TargetPercent;
	}

	if (FillBar)
	{
		FillBar->SetPercent(TargetPercent);
	}

	ApplyVisibility();
}

void UDemoNameplateWidget::SetPhaseHighlight(bool bPhase2)
{
	if (FillBar)
	{
		FillBar->SetFillColorAndOpacity(bPhase2 ? Phase2FillColor : FillColor);
	}
}

void UDemoNameplateWidget::SetHiddenByRule(bool bHidden)
{
	bHiddenByRule = bHidden;
	ApplyVisibility();
}

void UDemoNameplateWidget::ApplyVisibility()
{
	const bool bHideForFullHealth = bHideWhenFullHealth && TargetPercent >= 0.999f;
	const bool bShouldShow = !bHiddenByRule && !bHideForFullHealth;

	SetVisibility(bShouldShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UDemoNameplateWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!GhostBar)
	{
		return;
	}

	// 延迟层：只做一个"伤口"的视觉残影，不做停顿（头顶条要干净）
	if (GhostPercent > TargetPercent)
	{
		GhostPercent = FMath::Max(TargetPercent, GhostPercent - 0.9f * InDeltaTime);
		GhostBar->SetPercent(GhostPercent);
	}
	else if (GhostPercent < TargetPercent)
	{
		GhostPercent = TargetPercent;
		GhostBar->SetPercent(GhostPercent);
	}
}
