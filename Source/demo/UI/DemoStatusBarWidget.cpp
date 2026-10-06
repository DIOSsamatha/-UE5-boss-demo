#include "UI/DemoStatusBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"

// ==================================================================================
// 控件树
// ==================================================================================
TSharedRef<SWidget> UDemoStatusBarWidget::RebuildWidget()
{
	// ⚠ 顺序很关键：WidgetBlueprint 子类的控件树是在 UUserWidget::Initialize() 里才装上的
	//   （BGClass->InitializeWidget → WidgetTree = DuplicateObject(...)）。
	//   如果在 Initialize() 之前建树，整棵树会被它覆盖掉，血条就会"建了但看不见"。
	//   Initialize() 自带幂等保护，重复调用是安全的。
	Initialize();

	EnsureDefaultTree();
	return Super::RebuildWidget();
}

void UDemoStatusBarWidget::NativePreConstruct()
{
	Super::NativePreConstruct();
	// 设计器里不建树：WBP 的设计器树一旦被运行时逻辑写入，容易把临时控件序列化进资产。
	// 运行期由 RebuildWidget() 负责建树，所以这里不做任何事。
}

void UDemoStatusBarWidget::EnsureDefaultTree()
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

	// UMG 里已经自己搭了控件树（设计师手拖的）→ 尊重它，不覆盖
	if (WidgetTree->RootWidget)
	{
		bTreeReady = true;
		return;
	}

	// ---- 根：固定高度的 SizeBox（宽度由父容器决定）----
	RootSizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("StatusRootSizeBox"));
	if (!RootSizeBox)
	{
		return;
	}
	RootSizeBox->SetHeightOverride(BarHeight);

	RootOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("StatusRootOverlay"));
	if (!RootOverlay)
	{
		return;
	}
	RootSizeBox->AddChild(RootOverlay);

	// 往 Overlay 里加子控件时统一设成"铺满"，这样三层才真正叠在一起
	auto AddFullSlot = [this](UWidget* Child) -> void
	{
		if (!Child)
		{
			return;
		}
		RootOverlay->AddChildToOverlay(Child);
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Child->Slot))
		{
			OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
			OverlaySlot->SetVerticalAlignment(VAlign_Fill);
		}
	};

	// ---- 第 1 层：深色底框 ----
	BackBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StatusBackBorder"));
	if (BackBorder)
	{
		BackBorder->SetBrushColor(BackColor);
		AddFullSlot(BackBorder);
	}

	// ---- 第 2 层：延迟层（刚掉下去还没追上的那一截）----
	GhostBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("StatusGhostBar"));
	if (GhostBar)
	{
		GhostBar->SetPercent(1.f);
		GhostBar->SetFillColorAndOpacity(GhostColor);
		AddFullSlot(GhostBar);
	}

	// ---- 第 3 层：当前值 ----
	FillBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("StatusFillBar"));
	if (FillBar)
	{
		FillBar->SetPercent(1.f);
		FillBar->SetFillColorAndOpacity(FillColor);
		AddFullSlot(FillBar);
	}

	// ---- 第 4 层：数值文字（靠 OverlaySlot 的居中对齐实现居中，不用动 Justification）----
	ValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("StatusValueText"));
	if (ValueText)
	{
		FSlateFontInfo Font = ValueText->GetFont();
		Font.Size = FMath::Max(ValueFontSize, 6);
		ValueText->SetFont(Font);
		ValueText->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 1.f, 1.f, 0.95f)));
		ValueText->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f));
		ValueText->SetShadowOffset(FVector2D(1.f, 1.f));

		RootOverlay->AddChildToOverlay(ValueText);
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ValueText->Slot))
		{
			OverlaySlot->SetHorizontalAlignment(HAlign_Center);
			OverlaySlot->SetVerticalAlignment(VAlign_Center);
		}
	}

	WidgetTree->RootWidget = RootSizeBox;

	bTreeReady = true;
	RefreshVisuals();
}

// ==================================================================================
// 数据接口
// ==================================================================================
void UDemoStatusBarWidget::SetPercent(float InPercent, bool bInstant)
{
	const float NewPercent = FMath::Clamp(InPercent, 0.f, 1.f);

	// HUD 是每帧轮询数值的 → 必须区分"值真的变了"和"只是又读了一次同样的值"。
	// 不区分的话，掉血时每帧都会把延迟层的停顿计时器重置，金色条永远追不上来。
	const bool bChanged = !FMath::IsNearlyEqual(NewPercent, TargetPercent, 0.0005f);
	TargetPercent = NewPercent;

	if (bInstant || GhostCatchUpSpeed <= 0.f)
	{
		GhostPercent = TargetPercent;
		GhostHoldRemaining = 0.f;
		bVisualDirty = true;
	}
	else if (!bChanged)
	{
		// 值没变：正在进行的"停顿 → 追赶"动画保持不动
	}
	else if (GhostPercent < TargetPercent)
	{
		// 回血：延迟层立刻跟上，否则会出现"血回满了金色条还停在那儿"的怪现象
		GhostPercent = TargetPercent;
		GhostHoldRemaining = 0.f;
		bVisualDirty = true;
	}
	else
	{
		// 掉血：先停 GhostHoldTime，停顿结束后才开始往前追（这段停顿就是"看得见掉了多少"）
		GhostHoldRemaining = GhostHoldTime;
		bVisualDirty = true;
	}

	if (bVisualDirty)
	{
		RefreshVisuals();
	}
}

void UDemoStatusBarWidget::SetValueAndMax(float InValue, float InMax, bool bInstant)
{
	const bool bValueChanged = !bHasValuePair
		|| !FMath::IsNearlyEqual(InValue, LastValue, 0.5f)
		|| !FMath::IsNearlyEqual(InMax, LastMax, 0.5f);

	bHasValuePair = true;
	LastValue = InValue;
	LastMax = InMax;

	// 文字里的数字是取整显示的，只有整数位变了才需要重设文本
	if (bValueChanged)
	{
		bVisualDirty = true;
	}

	const float Percent = (InMax > KINDA_SMALL_NUMBER) ? (InValue / InMax) : 0.f;
	SetPercent(Percent, bInstant);
}

void UDemoStatusBarWidget::SetLabel(const FText& InLabel)
{
	CachedLabel = InLabel;
	bHasValuePair = false;
	bVisualDirty = true;
	RefreshVisuals();
}

void UDemoStatusBarWidget::SetBarColors(FLinearColor InFillColor, FLinearColor InGhostColor)
{
	FillColor = InFillColor;
	GhostColor = InGhostColor;

	if (FillBar)
	{
		FillBar->SetFillColorAndOpacity(FillColor);
	}
	if (GhostBar)
	{
		GhostBar->SetFillColorAndOpacity(GhostColor);
	}
}

void UDemoStatusBarWidget::SetShowValueText(bool bShow)
{
	bShowValueText = bShow;
	bVisualDirty = true;
	RefreshVisuals();
}

// ==================================================================================
// 每帧刷新
// ==================================================================================
void UDemoStatusBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!GhostBar && !FillBar)
	{
		return;
	}

	bool bNeedsRefresh = bVisualDirty;

	if (GhostPercent > TargetPercent && GhostCatchUpSpeed > 0.f)
	{
		if (GhostHoldRemaining > 0.f)
		{
			GhostHoldRemaining -= InDeltaTime;
		}
		else
		{
			GhostPercent = FMath::Max(TargetPercent, GhostPercent - GhostCatchUpSpeed * InDeltaTime);
			bNeedsRefresh = true;
		}
	}
	else if (GhostPercent < TargetPercent)
	{
		GhostPercent = TargetPercent;
		bNeedsRefresh = true;
	}

	if (bNeedsRefresh)
	{
		RefreshVisuals();
	}
}

void UDemoStatusBarWidget::RefreshVisuals()
{
	bVisualDirty = false;

	if (FillBar)
	{
		FillBar->SetPercent(FMath::Clamp(TargetPercent, 0.f, 1.f));
	}

	if (GhostBar)
	{
		GhostBar->SetPercent(FMath::Clamp(FMath::Max(GhostPercent, TargetPercent), 0.f, 1.f));
	}

	if (!ValueText)
	{
		return;
	}

	ValueText->SetVisibility(bShowValueText ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	if (!CachedLabel.IsEmpty())
	{
		ValueText->SetText(CachedLabel);
	}
	else if (bHasValuePair)
	{
		ValueText->SetText(bAppendMaxValue
			? FText::FromString(FString::Printf(TEXT("%d / %d"),
				FMath::RoundToInt(LastValue), FMath::RoundToInt(LastMax)))
			: FText::FromString(FString::Printf(TEXT("%d"), FMath::RoundToInt(LastValue))));
	}

	OnStatusBarUpdated(TargetPercent, GhostPercent);
}
