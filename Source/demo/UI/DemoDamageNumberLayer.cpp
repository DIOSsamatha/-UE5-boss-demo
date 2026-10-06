#include "UI/DemoDamageNumberLayer.h"

#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"

// ==================================================================================
// 控件树
// ==================================================================================
TSharedRef<SWidget> UDemoDamageNumberLayer::RebuildWidget()
{
	// 先 Initialize()：蓝图子类的 WidgetTree 在这一步才装上（见 UDemoStatusBarWidget 的注释）
	Initialize();

	EnsureDefaultTree();
	return Super::RebuildWidget();
}

void UDemoDamageNumberLayer::EnsureDefaultTree()
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

	if (WidgetTree->RootWidget)
	{
		bTreeReady = true;
		return;
	}

	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("DamageNumberCanvas"));
	if (!RootCanvas)
	{
		return;
	}

	// 飘字只是装饰，绝不能吃掉鼠标/触摸输入
	RootCanvas->SetVisibility(ESlateVisibility::HitTestInvisible);

	WidgetTree->RootWidget = RootCanvas;
	bTreeReady = true;
}

UTextBlock* UDemoDamageNumberLayer::AcquireTextBlock()
{
	// 池子里还有闲置的 → 直接拿一个
	if (TextPool.Num() < MaxActiveNumbers)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		if (!Text)
		{
			return nullptr;
		}

		FSlateFontInfo Font = Text->GetFont();
		Font.Size = FMath::Max(FontSize, 8);
		Text->SetFont(Font);
		Text->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f));
		Text->SetShadowOffset(FVector2D(1.f, 1.f));
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);

		UCanvasPanelSlot* CanvasSlot = RootCanvas->AddChildToCanvas(Text);
		if (CanvasSlot)
		{
			CanvasSlot->SetAutoSize(true);
			CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		}

		TextPool.Add(Text);
		return Text;
	}

	// 池子满了 → 轮转复用（保证同一时刻最多 MaxActiveNumbers 条，不会无限堆控件）
	if (TextPool.Num() == 0)
	{
		return nullptr;
	}
	NextPoolIndex = (NextPoolIndex + 1) % TextPool.Num();
	return TextPool[NextPoolIndex];
}

// ==================================================================================
// 生成一条飘字
// ==================================================================================
void UDemoDamageNumberLayer::SpawnDamageNumber(const FVector& WorldLocation, float Damage,
	EDemoHitResultType ResultType, EDemoDamageSource DamageSource, bool bIsIncoming)
{
	EnsureDefaultTree();
	if (!RootCanvas)
	{
		return;
	}

	UTextBlock* Text = AcquireTextBlock();
	if (!Text)
	{
		return;
	}

	const int32 Rounded = FMath::Max(1, FMath::RoundToInt(Damage));

	FString Display;
	FLinearColor Color;
	float Scale = 1.f;

	switch (ResultType)
	{
	case EDemoHitResultType::Blocked:
		// 格挡：显示的是实际扣掉的那一点点血
		Display = FString::Printf(TEXT("%d"), Rounded);
		Color = FLinearColor(0.58f, 0.75f, 0.98f, 1.f);
		Scale = 0.85f;
		break;

	case EDemoHitResultType::Critical:
		Display = FString::Printf(TEXT("%d!"), Rounded);
		Color = FLinearColor(1.f, 0.80f, 0.16f, 1.f);
		Scale = 1.4f;
		break;

	case EDemoHitResultType::Parried:
		Display = TEXT("弹反");
		Color = FLinearColor(0.35f, 0.95f, 1.f, 1.f);
		Scale = 1.3f;
		break;

	case EDemoHitResultType::Dodged:
		Display = TEXT("闪避");
		Color = FLinearColor(0.78f, 0.78f, 0.84f, 0.95f);
		break;

	default:
		Display = FString::Printf(TEXT("%d"), Rounded);
		// 打到敌人 = 暖白；自己挨打 = 红
		Color = bIsIncoming
			? FLinearColor(1.f, 0.36f, 0.30f, 1.f)
			: FLinearColor(1.f, 0.96f, 0.86f, 1.f);
		break;
	}

	// 大招/落地冲击这类大数值更醒目一点，方便玩家建立"哪种招更疼"的直觉
	if (DamageSource == EDemoDamageSource::Ultimate || DamageSource == EDemoDamageSource::HeavyImpact)
	{
		Scale *= 1.15f;
	}

	FSlateFontInfo Font = Text->GetFont();
	const int32 BaseSize = FMath::Max(FontSize, 8);
	Font.Size = FMath::Max(8, FMath::RoundToInt(BaseSize * Scale));
	Text->SetFont(Font);
	Text->SetText(FText::FromString(Display));
	Text->SetColorAndOpacity(FSlateColor(Color));
	Text->SetVisibility(ESlateVisibility::HitTestInvisible);

	FDamageNumber Entry;
	Entry.Text = Text;
	Entry.WorldLocation = WorldLocation;
	Entry.BaseColor = Color;
	Entry.Elapsed = 0.f;
	Entry.Lateral = FVector2D(FMath::FRandRange(-LateralSpread, LateralSpread), 0.f);

	// 同一个控件可能已经有一条在飘 → 顶掉旧的，避免两段动画打架
	for (int32 Index = ActiveNumbers.Num() - 1; Index >= 0; --Index)
	{
		if (ActiveNumbers[Index].Text.Get() == Text)
		{
			ActiveNumbers.RemoveAt(Index);
		}
	}

	ActiveNumbers.Add(Entry);
}

void UDemoDamageNumberLayer::ClearAllNumbers()
{
	ActiveNumbers.Reset();

	for (const TObjectPtr<UTextBlock>& Text : TextPool)
	{
		if (Text)
		{
			Text->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

// ==================================================================================
// 每帧：跟随世界坐标 + 上飘 + 淡出
// ==================================================================================
void UDemoDamageNumberLayer::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (ActiveNumbers.Num() == 0)
	{
		return;
	}

	APlayerController* PlayerController = GetOwningPlayer();
	if (!PlayerController)
	{
		ClearAllNumbers();
		return;
	}

	const float Lifetime = FMath::Max(NumberLifetime, 0.05f);

	for (int32 Index = ActiveNumbers.Num() - 1; Index >= 0; --Index)
	{
		FDamageNumber& Entry = ActiveNumbers[Index];
		UTextBlock* Text = Entry.Text.Get();
		if (!Text)
		{
			ActiveNumbers.RemoveAtSwap(Index);
			continue;
		}

		Entry.Elapsed += InDeltaTime;
		if (Entry.Elapsed >= Lifetime)
		{
			Text->SetVisibility(ESlateVisibility::Collapsed);
			ActiveNumbers.RemoveAtSwap(Index);
			continue;
		}

		const float Alpha = Entry.Elapsed / Lifetime;

		FVector2D ScreenPosition = FVector2D::ZeroVector;
		if (!PlayerController->ProjectWorldLocationToScreen(Entry.WorldLocation, ScreenPosition, true))
		{
			// 在相机背后 → 这一帧先藏起来（不要瞬间跳到屏幕另一边）
			Text->SetVisibility(ESlateVisibility::Hidden);
			continue;
		}
		Text->SetVisibility(ESlateVisibility::HitTestInvisible);

		// ⚠ 与锁定准星同一个坑（2026-09-26）：ProjectWorldLocationToScreen 给的是【视口像素】，
		//   而 CanvasPanelSlot 的位置是【控件本地单位】，两者差一个 DPI / 视口缩放。
		//   不除这个缩放，飘字会整体朝左上角偏（本项目默认 DPI 曲线在 1080 高时缩放 ≈0.76）。
		const float ViewportScale = UWidgetLayoutLibrary::GetViewportScale(this);
		if (ViewportScale > KINDA_SMALL_NUMBER)
		{
			ScreenPosition /= ViewportScale;
		}

		ScreenPosition += Entry.Lateral;
		ScreenPosition.Y -= RiseDistance * Alpha;

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Text->Slot))
		{
			CanvasSlot->SetPosition(ScreenPosition);
		}

		// 后半程明显淡出，读起来更轻
		const float Fade = FMath::Clamp(1.f - FMath::Max(0.f, (Alpha - 0.45f) / 0.55f), 0.f, 1.f);
		Text->SetColorAndOpacity(FSlateColor(Entry.BaseColor * Fade));
	}
}
