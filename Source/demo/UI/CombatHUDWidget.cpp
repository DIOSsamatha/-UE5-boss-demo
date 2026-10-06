#include "UI/CombatHUDWidget.h"
#include "UI/DemoStatusBarWidget.h"
#include "UI/DemoDamageNumberLayer.h"

#include "Core/DemoCharacterBase.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoGameplayTags.h"
#include "Core/DemoCombatTuning.h"
#include "Boss/BossCharacterBase.h"
#include "Boss/BossBrainComponent.h"
#include "Player/FightPlayerCharacter.h"
#include "Player/LockOnComponent.h"

#include "AbilitySystemComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"

namespace
{
	// ------------------------------------------------------------------
	// 整屏闪的径向渐变贴图（一次性生成，给 ScreenFlash 用）
	//
	// ★ 为什么必须有它：ScreenFlash 是 UImage，而【没有设置任何 brush 的 UImage
	//   画出来就是一块铺满全屏的实心矩形】。于是每一次 FlashScreen() 在画面上
	//   都是"一整块半透明色板"，而不是"闪了一下"。玩家两次反馈"稳心时出现
	//   一块灰色的板"—— 0.92/0.95/1.0 的近白色 × 28% 不透明度，正好是"灰板"。
	//
	//   贴上这张中心亮、四周透明的径向渐变之后，它才真的是"画面中央泛一下光"。
	//   剩下还在用的闪（暴击 / 完美闪避 / 普通闪避）都走这一层。
	// ------------------------------------------------------------------
	UTexture2D* MakeRadialFlashTexture()
	{
		const int32 Size = 128;
		UTexture2D* Tex = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8, TEXT("DemoFlashGradient"));
		if (!Tex)
		{
			return nullptr;
		}

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				// 中心 (0.5,0.5) → 1，边缘 → 0
				const float DX = (X + 0.5f) / static_cast<float>(Size) * 2.f - 1.f;
				const float DY = (Y + 0.5f) / static_cast<float>(Size) * 2.f - 1.f;
				const float R = FMath::Sqrt(DX * DX + DY * DY);
				// pow 1.6 让中心更集中、边缘过渡更软（线性衰减会看到明显的圆盘边）
				const float A = FMath::Pow(FMath::Clamp(1.f - R, 0.f, 1.f), 1.6f);
				Pixels[Y * Size + X] = FColor(255, 255, 255,
					static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(A * 255.f), 0, 255)));
			}
		}

		// ⚠ 用 BulkData 直接写像素而不是 UpdateTextureRegions：
		//   后者要引 FUpdateTextureRegion2D（RenderResource.h），这里少一个依赖更稳。
		//   ⚠ TBulkData 在 UE5.7 上是 Lock(LockFlags) / Unlock()，
		//     【没有】LockBulkData()/UnlockBulkData()（那是我记混的旧 API，会报 C2039）。
		FTexturePlatformData* PlatformData = Tex->GetPlatformData();
		if (PlatformData && PlatformData->Mips.Num() > 0)
		{
			FBulkData& Bulk = PlatformData->Mips[0].BulkData;
			void* Data = Bulk.Lock(LOCK_READ_WRITE);
			if (Data)
			{
				FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			}
			Bulk.Unlock();
		}
		Tex->Filter = TF_Bilinear;
		Tex->SRGB = true;
		Tex->NeverStream = true;
		Tex->UpdateResource();
		return Tex;
	}
}

namespace
{
	// 反馈大字的字号（不要再各处写魔法数字了）
	constexpr int32 FeedbackFontSize = 34;
	constexpr int32 DeathFontSize = 58;
	constexpr int32 NameFontSize = 16;

	// 稍微粗一点的默认字体：用 TextBlock 自带字体的普通字型，只改字号不引资产
	void ApplyFontSize(UTextBlock* Text, int32 Size)
	{
		if (!Text)
		{
			return;
		}
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = FMath::Max(Size, 6);
		Text->SetFont(Font);
	}

	void ApplyOutline(UTextBlock* Text, const FLinearColor& Color, float Offset = 2.f)
	{
		if (!Text)
		{
			return;
		}
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f));
		Text->SetShadowOffset(FVector2D(Offset, Offset));
	}
}

// ==================================================================================
// 玩家 HUD
// ==================================================================================
TSharedRef<SWidget> UCombatHUDWidget::RebuildWidget()
{
	// ⚠ 必须先 Initialize()：WidgetBlueprint 子类的控件树是在 Initialize 里才装上的，
	//   提前建树会被 InitializeWidget 覆盖（血条会"建了但看不见"）。
	Initialize();

	if (bBuildDefaultLayout)
	{
		EnsureDefaultTree();
	}

	return Super::RebuildWidget();
}

void UCombatHUDWidget::EnsureDefaultTree()
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

	// 设计师已经在 UMG 里搭好了 → 尊重它
	if (WidgetTree->RootWidget)
	{
		bTreeReady = true;
		return;
	}

	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HUDRootCanvas"));
	if (!RootCanvas)
	{
		return;
	}
	RootCanvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// ---------------- 全屏闪光层（必须是第一个子控件 = 画在最底层）----------------
	//
	// 完美闪避 / 完美弹反时整屏闪一下。放在最前面加入，是为了让它当"背景泛光"，
	// 而不是把血条和飘字糊住 —— Canvas 的子控件按加入顺序绘制，越晚加越在上层。
	//
	// 为什么用一张全屏 Image 而不是 PostProcess：Image 不依赖任何后处理资产，
	// 纯 C++ 就能控制颜色与淡出曲线，零配置就能看到效果。
	// 想换成真正的后处理泛光，覆写 OnPerfectParry / OnPerfectDodge 蓝图事件即可。
	ScreenFlash = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ScreenFlash"));
	if (ScreenFlash)
	{
		// ⚠ 必须给 brush！不设 brush 的 UImage = 一块铺满全屏的实心色板（"灰板"的根因）。
		//   贴上径向渐变后，FlashScreen() 才是"画面中央泛一下光"而不是"盖一块板"。
		if (UTexture2D* Gradient = MakeRadialFlashTexture())
		{
			ScreenFlash->SetBrushFromTexture(Gradient);
		}
		else
		{
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[HUD] ⚠ 整屏闪渐变贴图生成失败，FlashScreen 会退化成实心色板"));
		}
		ScreenFlash->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.f));
		ScreenFlash->SetVisibility(ESlateVisibility::Collapsed);

		if (UCanvasPanelSlot* FlashSlot = RootCanvas->AddChildToCanvas(ScreenFlash))
		{
			FlashSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			FlashSlot->SetOffsets(FMargin(0.f));
		}
	}

	// ---------------- 左下：状态区 ----------------
	USizeBox* StatusPanel = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("StatusPanel"));
	StatusPanel->SetWidthOverride(StatusPanelWidth);

	UVerticalBox* StatusBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StatusBox"));
	StatusPanel->AddChild(StatusBox);

	auto AddBar = [this, StatusBox](const TCHAR* Name, float Height, FLinearColor Fill, FLinearColor Ghost,
		FLinearColor Back, int32 FontSize) -> UDemoStatusBarWidget*
	{
		UDemoStatusBarWidget* Bar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), Name);
		if (!Bar || !StatusBox)
		{
			return Bar;
		}

		// 这些属性必须在控件被"取用"（TakeWidget → RebuildWidget）之前设置好：
		// 状态条是在自己的 RebuildWidget 里按这些值建树的。
		Bar->BarHeight = Height;
		Bar->FillColor = Fill;
		Bar->GhostColor = Ghost;
		Bar->BackColor = Back;
		Bar->ValueFontSize = FontSize;
		Bar->bShowValueText = true;

		if (UVerticalBoxSlot* VSlot = StatusBox->AddChildToVerticalBox(Bar))
		{
			VSlot->SetHorizontalAlignment(HAlign_Fill);
			VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		}
		return Bar;
	};

	HealthBar = AddBar(TEXT("HealthBar"), 24.f,
		FLinearColor(0.78f, 0.13f, 0.11f, 1.f),   // 当前血量：红
		FLinearColor(0.96f, 0.88f, 0.58f, 0.92f), // 刚掉的那截：金
		FLinearColor(0.03f, 0.02f, 0.02f, 0.82f), 14);

	if (bShowStaminaBar)
	{
		StaminaBar = AddBar(TEXT("StaminaBar"), 12.f,
			FLinearColor(0.20f, 0.68f, 0.30f, 1.f),   // 耐力：绿
			FLinearColor(0.75f, 0.92f, 0.60f, 0.85f),
			FLinearColor(0.02f, 0.03f, 0.02f, 0.82f), 11);
	}

	if (bShowPoiseBar)
	{
		PoiseBar = AddBar(TEXT("PoiseBar"), 10.f,
			FLinearColor(0.42f, 0.52f, 0.86f, 1.f),   // 韧性：蓝
			FLinearColor(0.85f, 0.85f, 0.95f, 0.85f),
			FLinearColor(0.02f, 0.02f, 0.04f, 0.82f), 10);
	}

	if (bShowPostureBar)
	{
		PostureBar = AddBar(TEXT("PostureBar"), 10.f,
			FLinearColor(0.98f, 0.78f, 0.22f, 1.f),   // 架势：金（= "还能挡多久"）
			FLinearColor(1.f, 0.95f, 0.70f, 0.85f),
			FLinearColor(0.04f, 0.03f, 0.01f, 0.82f), 10);
		PostureBar->SetLabel(FText::FromString(TEXT("架势")));
	}

	// ---------------- 回血"酒"持有量（"酒 ×N/M"）----------------
	// 紧跟在状态条下面，常驻显示玩家当前带了几瓶酒 / 上限。
	// 真正"喝下去回血"的绿色反馈由 UGA_PlayerUseHeal::ApplyHeal 通过 FlashScreen + ShowFeedbackText 完成，
	// 这里只负责"还剩余几瓶"的读数，让玩家知道要不要省着喝。
	if (bShowHealItemText)
	{
		HealItemText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("HealItemText"));
		if (HealItemText)
		{
			ApplyFontSize(HealItemText, 18);
			ApplyOutline(HealItemText, FLinearColor(0.3f, 1.f, 0.45f, 1.f));
			HealItemText->SetText(FText::FromString(TEXT("酒 ×0/0")));
			HealItemText->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 1.f, 0.7f, 1.f)));

			if (UVerticalBoxSlot* VSlot = StatusBox->AddChildToVerticalBox(HealItemText))
			{
				VSlot->SetHorizontalAlignment(HAlign_Left);
				VSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
			}
		}
	}

	if (UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(StatusPanel))
	{
		// 锚在屏幕左下角，对齐方式也是左下 → 改变窗口分辨率时始终贴角
		PanelSlot->SetAnchors(FAnchors(0.f, 1.f, 0.f, 1.f));
		PanelSlot->SetAlignment(FVector2D(0.f, 1.f));
		PanelSlot->SetAutoSize(true);
		PanelSlot->SetPosition(StatusPanelOffset);
	}

	// ---------------- 全屏：伤害飘字层（放在状态条之后 = 画在它上面）----------------
	if (bShowDamageNumbers)
	{
		DamageNumberLayer = WidgetTree->ConstructWidget<UDemoDamageNumberLayer>(UDemoDamageNumberLayer::StaticClass(), TEXT("DamageNumberLayer"));
		if (DamageNumberLayer)
		{
			if (UCanvasPanelSlot* LayerSlot = RootCanvas->AddChildToCanvas(DamageNumberLayer))
			{
				// 铺满全屏：飘字用的是屏幕绝对坐标，这一层的画布必须和屏幕一样大
				LayerSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
				LayerSlot->SetOffsets(FMargin(0.f));
			}
		}
	}

	// ---------------- 屏幕中上：命中反馈大字 ----------------
	FeedbackText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("FeedbackText"));
	if (FeedbackText)
	{
		ApplyFontSize(FeedbackText, FeedbackFontSize);
		ApplyOutline(FeedbackText, FLinearColor::White);
		FeedbackText->SetVisibility(ESlateVisibility::Collapsed);

		if (UCanvasPanelSlot* TextSlot = RootCanvas->AddChildToCanvas(FeedbackText))
		{
			TextSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			TextSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			TextSlot->SetAutoSize(true);
			TextSlot->SetPosition(FVector2D(0.f, -70.f));
		}
	}

	// ---------------- 屏幕中心：阵亡提示（默认隐藏）----------------
	DeathText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DeathText"));
	if (DeathText)
	{
		ApplyFontSize(DeathText, DeathFontSize);
		ApplyOutline(DeathText, FLinearColor(0.86f, 0.16f, 0.14f, 1.f), 3.f);
		DeathText->SetText(FText::FromString(TEXT("你 已 阵 亡")));
		DeathText->SetVisibility(ESlateVisibility::Collapsed);

		if (UCanvasPanelSlot* TextSlot = RootCanvas->AddChildToCanvas(DeathText))
		{
			TextSlot->SetAnchors(FAnchors(0.5f, 0.5f));
			TextSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			TextSlot->SetAutoSize(true);
		}
	}

	// ---------------- 锁定准星（画在最上层）----------------
	//
	// 为什么是"投影到屏幕的 2D 准星"而不是目标头顶的世界空间 Widget：
	//   · 不会被 BOSS 的模型、特效、其他描边盖住 —— 锁定状态必须永远可读；
	//   · 目标贴到脸上时不会因为透视被放大到糊屏；
	//   · 复用 HUD 已有的 ProjectWorldLocationToScreen 路径，不用给每个敌人挂组件。
	//
	// 形状用四角括号而不是一个圆：括号留出中心视野，不会挡住 BOSS 的动作读招。
	// 全部由细长矩形拼出来，所以不需要任何贴图资产。
	if (bShowLockOnMarker)
	{
		LockOnMarkerRoot = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("LockOnMarkerRoot"));
		if (LockOnMarkerRoot)
		{
			LockOnMarkerRoot->SetVisibility(ESlateVisibility::Collapsed);

			if (UCanvasPanelSlot* MarkerSlot = RootCanvas->AddChildToCanvas(LockOnMarkerRoot))
			{
				// 锚在左上角 + 绝对像素定位：投影算出来就是屏幕绝对坐标，
				// 用比例锚点会再被拉伸一次，位置就飘了。
				MarkerSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
				MarkerSlot->SetAlignment(FVector2D(0.5f, 0.5f));
				MarkerSlot->SetAutoSize(false);
				MarkerSlot->SetSize(FVector2D(LockOnMarkerSize, LockOnMarkerSize));
			}

			const float S = LockOnMarkerSize;

			// 括号臂长 / 线宽 / 内缩，按尺寸等比缩放 → 改 LockOnMarkerSize 时形状不跑
			const float Arm = FMath::Max(S * 0.24f, 4.f);
			const float Thick = FMath::Max(S * 0.045f, 1.f);
			const float Inset = FMath::Max(S * 0.10f, 1.f);

			// 名字刻意和上面玩家血条那三个的 AddBar 区分开：
			// 同一函数里重复声明同名 lambda 会触发 C4456（本项目把警告当错误）
			auto AddReticleBar = [this, S](FVector2D Pos, FVector2D Size) -> UImage*
			{
				UImage* Bar = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
				if (!Bar)
				{
					return nullptr;
				}

				Bar->SetColorAndOpacity(LockOnMarkerColor);
				Bar->SetVisibility(ESlateVisibility::HitTestInvisible);

				if (UCanvasPanelSlot* Slot = LockOnMarkerRoot->AddChildToCanvas(Bar))
				{
					Slot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
					Slot->SetAlignment(FVector2D(0.f, 0.f));
					Slot->SetAutoSize(false);
					Slot->SetPosition(Pos);
					Slot->SetSize(Size);
				}

				LockOnMarkerBars.Add(Bar);
				return Bar;
			};

			// 四个角：每角一条横线 + 一条竖线
			const float FarX = S - Inset - Arm;
			const float FarY = S - Inset - Arm;

			AddReticleBar(FVector2D(Inset, Inset), FVector2D(Arm, Thick));                 // 左上 ·
			AddReticleBar(FVector2D(Inset, Inset), FVector2D(Thick, Arm));                 // 左上 |
			AddReticleBar(FVector2D(S - Inset - Arm, Inset), FVector2D(Arm, Thick));       // 右上 ·
			AddReticleBar(FVector2D(S - Inset - Thick, Inset), FVector2D(Thick, Arm));     // 右上 |
			AddReticleBar(FVector2D(Inset, S - Inset - Thick), FVector2D(Arm, Thick));     // 左下 ·
			AddReticleBar(FVector2D(Inset, FarY), FVector2D(Thick, Arm));                  // 左下 |
			AddReticleBar(FVector2D(FarX, S - Inset - Thick), FVector2D(Arm, Thick));      // 右下 ·
			AddReticleBar(FVector2D(S - Inset - Thick, FarY), FVector2D(Thick, Arm));      // 右下 |
		}
	}

	WidgetTree->RootWidget = RootCanvas;
	bTreeReady = true;
}
// ==================================================================================
// 绑定
// ==================================================================================
void UCombatHUDWidget::BindToCharacter(ADemoCharacterBase* Character)
{
	BoundCharacter = Character;

	if (!Character)
	{
		return;
	}

	// 数值靠每帧轮询（见 NativeTick），所以这里只订阅"离散事件"。
	// 用动态委托（AddDynamic）而不是普通委托：动态委托内部是弱引用，
	// 控件被销毁后不会回调到野指针。
	Character->OnCharacterDeath.AddDynamic(this, &UCombatHUDWidget::HandleCharacterDeath);
	Character->OnDamagedDetailed.AddDynamic(this, &UCombatHUDWidget::HandleDamagedDetailed);
	Character->OnPoiseBroken.AddDynamic(this, &UCombatHUDWidget::HandlePoiseBroken);
	Character->OnHitLanded.AddDynamic(this, &UCombatHUDWidget::HandleHitLanded);

	// 弹反 / 闪避是 GAS 事件（ApplyHit 发给双方），从 ASC 上听。
	//
	// 四类事件分开订阅，因为"完美"和"普通"要给出完全不同强度的反馈：
	//   Event.Parry.Success  → 弹到了（切成反击动作、弹一条字）
	//   Event.Parry.Perfect   → 弹得很准（强顿帧 + 慢动作 + 金光 + 整屏闪）
	//   Event.Dodge.Perfect   → 卡着出招那一刻闪开（子弹时间 + 青白闪 + 大字）
	//   Event.Dodge.Normal    → 只是无敌帧内（轻提示，不抢戏）
	if (UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(Character))
	{
		ASC->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Parry_Success)
			.AddUObject(this, &UCombatHUDWidget::HandleParryEvent);
		ASC->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Parry_Perfect)
			.AddUObject(this, &UCombatHUDWidget::HandlePerfectParryEvent);
		ASC->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Dodge_Perfect)
			.AddUObject(this, &UCombatHUDWidget::HandleDodgeEvent);
		ASC->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Dodge_Normal)
			.AddUObject(this, &UCombatHUDWidget::HandleNormalDodgeEvent);
		ASC->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Clash)
			.AddUObject(this, &UCombatHUDWidget::HandleClashEvent);
	}

	// 立刻同步一次，别等第一个 50ms
	SyncAccumulator = SyncInterval;
	SyncFromCharacter();
}

// ==================================================================================
// 事件
// ==================================================================================
void UCombatHUDWidget::HandleCharacterDeath()
{
	if (!bDeathShown)
	{
		bDeathShown = true;

		if (DeathText)
		{
			DeathText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
	}

	OnPlayerDeath();
}

void UCombatHUDWidget::HandlePoiseBroken()
{
	ShowFeedbackText(FText::FromString(TEXT("破 防")), FLinearColor(1.f, 0.78f, 0.20f, 1.f), 1.4f);
	OnPlayerPoiseBroken();
}

void UCombatHUDWidget::HandleDamagedDetailed(float Damage, EDemoHitResultType ResultType,
	EDemoDamageSource DamageSource, FVector HitDirection)
{
	// 1) 飘字（贴在玩家身上）
	if (DamageNumberLayer && bShowDamageNumbers && BoundCharacter)
	{
		const FVector Where = BoundCharacter->GetActorLocation() + FVector(0.f, 0.f, 110.f);
		DamageNumberLayer->SpawnDamageNumber(Where, Damage, ResultType, DamageSource, true);
	}

	// 2) 居中反馈大字：只有"非普通命中"才值得弹，否则每次挨打都刷屏
	switch (ResultType)
	{
	case EDemoHitResultType::Blocked:
		ShowFeedbackText(FText::FromString(TEXT("格 挡")), FLinearColor(0.60f, 0.78f, 1.f, 1.f));
		break;
	case EDemoHitResultType::Critical:
		ShowFeedbackText(FText::FromString(TEXT("被 破 防")), FLinearColor(1.f, 0.45f, 0.25f, 1.f));
		break;
	case EDemoHitResultType::Parried:
		ShowFeedbackText(FText::FromString(TEXT("弹 反 成 功")), FLinearColor(0.35f, 0.95f, 1.f, 1.f));
		break;
	default:
		break;
	}

	OnHitFeedback(ResultType, BoundCharacter ? BoundCharacter->GetActorLocation() : FVector::ZeroVector);
	OnPlayerDamagedDetailed(Damage, ResultType, DamageSource, HitDirection);
}

void UCombatHUDWidget::HandleHitLanded(AActor* HitActor, float Damage, EDemoHitResultType ResultType, FVector HitLocation)
{
	// 玩家打中敌人：飘字归 BOSS 血条那边画（它才是"敌人血条"的拥有者），
	// 这里只把事件转给蓝图，方便做打击音效 / 屏幕边闪。
	OnHitFeedback(ResultType, HitLocation);

	// ---- 只有"真的砍进去了"才给正反馈 ----
	//
	// OnHitLanded 是【攻击方】的回调，所以 ResultType 可能是"被弹反 / 被闪避"：
	// 那时候玩家是吃亏的一方，再给镜头冲击等于奖励失误，手感会彻底错乱。
	// 所以先分流：
	//   · Normal / Critical → 给完整的"砍实了"反馈
	//   · Blocked           → 给"闷"的一下（短、钝，没有回响）
	//   · Parried / Dodged  → 什么都不给（各自的失败表现走别的链路）
	const bool bCritical = (ResultType == EDemoHitResultType::Critical);
	const bool bLanded = (ResultType == EDemoHitResultType::Normal) || bCritical;
	const bool bBlocked = (ResultType == EDemoHitResultType::Blocked);
	if (!bLanded && !bBlocked)
	{
		return;
	}

	// 落地镜头微冲击：每一下命中都给一下很轻的"砸实了"反馈，
	// 解决"砍在 BOSS 身上没手感"的问题。
	//   · ApplyCameraPunch：拉近式挤压感（静态的"砸实"，仅非锁定时）
	//   · PulseCameraFOV：扩 FOV 的怼脸感（锁定安全 —— 打 BOSS 时的主要冲击来源）
	//   · PlayHitImpactShake：旋转微震，强度随伤害缩放（动态的"命中回馈"，锁定安全）
	// 三者叠加，连段时叠起来是节奏感而不是眩晕。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		if (bBlocked)
		{
			// 被挡住：短、钝、不扩 FOV。
			// 玩家要能"闭着眼"分辨出"这一下被吃住了" —— 靠的就是这种质感差异。
			Player->ApplyCameraPunch(DemoBalance::PlayerHitBlockedPunchLength,
				DemoBalance::PlayerHitBlockedPunchDuration);
			Player->PlayHitImpactShake(Damage * 0.5f, DemoBalance::PlayerHitBlockedShakeDuration);
			// Screen Shake 最低档：闷闷的一小下，和"砍进肉里"的震区分开
			Player->PlayHitScreenShake(DemoBalance::HitShakeBlockedIntensity);
		}
		else
		{
			Player->ApplyCameraPunch(
				bCritical ? DemoBalance::PlayerHitCritPunchLength : DemoBalance::PlayerHitPunchLength,
				bCritical ? DemoBalance::PlayerHitCritPunchDuration : DemoBalance::PlayerHitPunchDuration);

			Player->PlayHitImpactShake(Damage,
				bCritical ? DemoBalance::PlayerHitCritShakeDuration : DemoBalance::PlayerHitShakeDuration);

			// Screen Shake：基础档 + 伤害附加（越重的刀震得越狠，封顶见 DemoCombatTuning.h）。
			//   普攻四段 28/34/42/56 → 总强度 ≈ 0.66/0.69/0.72/0.77，暴击 1.0 起步 —— 连段有渐进感。
			const float DamageBonus = FMath::Min(Damage * DemoBalance::HitShakeDamagePerPoint,
				DemoBalance::HitShakeDamageBonusMax);
			Player->PlayHitScreenShake(
				(bCritical ? DemoBalance::HitShakeCritIntensity : DemoBalance::HitShakeNormalIntensity)
				+ DamageBonus);

			// ⚠ bAllowWhileLocked=true 是这次修复的关键：
			//   臂长冲击在锁定时被屏蔽，而打 BOSS 全程锁定 ——
			//   少了 FOV 这一道，锁定砍中 BOSS 就完全没有镜头反馈。
			Player->PulseCameraFOV(
				bCritical ? DemoBalance::PlayerHitCritFOVPulse : DemoBalance::PlayerHitFOVPulse,
				DemoBalance::PlayerHitFOVPulseDuration, /*bAllowWhileLocked=*/true);
		}

		// 命中音：打击感里声音占一半。位置传【命中点】，否则连段里玩家一直在移动，
		// 声音会从人这边飘出来、而火花在 BOSS 身上 —— 声画分离就没有"砸中"的感觉。
		Player->PlayHitSound(bCritical, HitLocation);
	}

	// 破防/暴击补一层极淡的暖色闪。
	// 只在"最重的那一击"给：每次命中都闪会让屏幕一直在抽搐，反而读不出轻重。
	if (bCritical)
	{
		FlashScreen(FLinearColor(1.f, 0.88f, 0.62f, 1.f), 0.14f, 0.16f);
	}
}

void UCombatHUDWidget::HandleParryEvent(const FGameplayEventData* Payload)
{
	// 只处理"我弹反了别人"（Target = 我）；我被别人弹反走的是另一套表现
	if (!Payload || !BoundCharacter)
	{
		return;
	}
	if (Payload->Target.Get() != static_cast<const AActor*>(BoundCharacter.Get()))
	{
		return;
	}

	if (DamageNumberLayer && bShowDamageNumbers)
	{
		DamageNumberLayer->SpawnDamageNumber(BoundCharacter->GetActorLocation() + FVector(0.f, 0.f, 130.f),
			0.f, EDemoHitResultType::Parried, EDemoDamageSource::Unknown, false);
	}

	ShowFeedbackText(FText::FromString(TEXT("弹 反 !")), FLinearColor(0.35f, 0.95f, 1.f, 1.f), 1.4f);

	// ⚠ 2026-10-06 删掉了这层整屏闪（原来是 FlashScreen(冷蓝 0.45,0.85,1 / 0.42s / 0.24)）。
	//   玩家反馈："被攻击时稳心会有灰色的板出现" —— 被攻击 → 反手弹开 = 稳心，
	//   而弹反/完美弹反这两层闪是同一次事件里【连着闪两下】的（两个 Handler 都会被触发），
	//   叠在一起就是一大片盖住屏幕的色板。稳心的反馈现在由镜头冲击 + "弹 反 !"/"完 美 弹 反 !"
	//   飘字 + 顿帧 + 弹反火花承担，不再糊屏。
	//   另外 ScreenFlash 现在挂的是径向渐变贴图（见 MakeRadialFlashTexture），
	//   剩下的闪（暴击 / 闪避）已经是"中央泛光"而不是色板。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		Player->ApplyCameraPunch(16.f, 0.22f);
	}

	OnParrySuccess();
}

// 完美弹反：判定窗前 0.12s 内弹到。
// 结构上它和 HandleParryEvent 会【同时】被触发（ApplyHit 两个事件都发），
// 所以这里只做"加强"的部分，不重复飘字，避免叠两层。
void UCombatHUDWidget::HandlePerfectParryEvent(const FGameplayEventData* Payload)
{
	if (!Payload || !BoundCharacter)
	{
		return;
	}
	if (Payload->Target.Get() != static_cast<const AActor*>(BoundCharacter.Get()))
	{
		return;
	}

	// ⚠ 2026-10-06 删掉了这层整屏闪（原来是 FlashScreen(金 1,0.82,0.30 / 0.42s / **0.45**)，全场最亮的一层）。
	//   玩家反馈"被攻击时稳心会有灰色的板出现"：45% 不透明度的近白金色铺满全屏，
	//   叠在"被攻击 → 完美弹反（= 稳心）"这条路径上就是一个盖住视野的大色板。
	//   镜头冲击（32 / 0.36）保留 —— 它的打击感比色板强得多，而且不会糊屏。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		Player->ApplyCameraPunch(32.f, 0.36f);
	}

	ShowFeedbackText(FText::FromString(TEXT("完 美 弹 反 !")),
		FLinearColor(1.f, 0.80f, 0.24f, 1.f), 1.6f);

	if (DamageNumberLayer && bShowDamageNumbers)
	{
		DamageNumberLayer->SpawnDamageNumber(BoundCharacter->GetActorLocation() + FVector(0.f, 0.f, 150.f),
			0.f, EDemoHitResultType::Parried, EDemoDamageSource::Unknown, false);
	}

	OnPerfectParry();
}

void UCombatHUDWidget::HandleClashEvent(const FGameplayEventData* Payload)
{
	// 拼刀是双方都收到的事件（ResolveClash 给 A、B 各发一份）。
	// 只处理"玩家参与了这次拼刀"的情况：玩家是发起方或被拼的一方都算。
	if (!Payload || !BoundCharacter)
	{
		return;
	}
	const AActor* Self = static_cast<const AActor*>(BoundCharacter.Get());
	if (Payload->Instigator.Get() != Self && Payload->Target.Get() != Self)
	{
		return;
	}

	// ⚠ 2026-10-06 这里原来有一记整屏闪：FlashScreen(FLinearColor(0.92f, 0.95f, 1.f, 1.f), 0.30f, 0.28f)
	//   （注释写的是"中性银白闪，用来和弹反的冷蓝、完美弹反的金区分"）——**已按用户要求删掉**。
	//   删的原因：它是所有整屏闪里**唯一不带明确色相**的一层（0.92/0.95/1.0 ≈ 纯白），
	//   28% 不透明度铺满全屏，在明亮的竞技场里读起来就是"一整块灰板盖在屏幕上"，
	//   根本不像闪光 —— 玩家反馈"被攻击时稳心会有灰色的板出现"。
	//   其余几层都带明确色相（弹反=冷蓝 / 完美弹反=金 / 完美闪避=青白 / 暴击=暖金），
	//   不会被误认成灰板，所以保留。
	//   稳心的反馈现在只靠下面两样：镜头轻推 + "稳 心 !" 飘字。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		Player->ApplyCameraPunch(18.f, 0.24f);
	}

	ShowFeedbackText(FText::FromString(TEXT("稳 心 !")), FLinearColor(0.85f, 0.90f, 1.f, 1.f), 1.3f);

	OnClash();
}

void UCombatHUDWidget::HandleDodgeEvent(const FGameplayEventData* Payload)
{
	if (!Payload || !BoundCharacter)
	{
		return;
	}
	if (Payload->Target.Get() != static_cast<const AActor*>(BoundCharacter.Get()))
	{
		return;
	}

	if (DamageNumberLayer && bShowDamageNumbers)
	{
		DamageNumberLayer->SpawnDamageNumber(BoundCharacter->GetActorLocation() + FVector(0.f, 0.f, 130.f),
			0.f, EDemoHitResultType::Dodged, EDemoDamageSource::Unknown, false);
	}

	ShowFeedbackText(FText::FromString(TEXT("完 美 闪 避")), FLinearColor(0.85f, 0.90f, 1.f, 1.f), 1.2f);

	// 青白色整屏泛光：和"子弹时间"是同一套反馈，视觉上互相印证
	FlashScreen(FLinearColor(0.78f, 0.92f, 1.f, 1.f), 0.30f, 0.32f);

	// 完美闪避的镜头冲击比完美弹反更轻、更"滑"：
	// 闪避是位移动作，镜头本身已经在跟着角色走了，再压太狠会晕。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		Player->ApplyCameraPunch(18.f, 0.28f);
	}

	OnPerfectDodge();
}

// 普通闪避：只吃到无敌帧（闪避后半段才被打到）。
// 刻意不给整屏闪光 —— 否则"完美"和"普通"在视觉上就没有区别了。
void UCombatHUDWidget::HandleNormalDodgeEvent(const FGameplayEventData* Payload)
{
	if (!Payload || !BoundCharacter)
	{
		return;
	}
	if (Payload->Target.Get() != static_cast<const AActor*>(BoundCharacter.Get()))
	{
		return;
	}

	ShowFeedbackText(FText::FromString(TEXT("闪 避")),
		FLinearColor(0.62f, 0.72f, 0.85f, 1.f), 0.7f);

	// 普通闪避（不在完美窗内）也给一点轻反馈：淡蓝微闪 + 短促镜头轻推。
	// 否则"按了 Shift 但好像什么都没发生"，闪避的手感就空了。
	// 远弱于完美闪避（青白闪 0.32 / 镜头 18），只做"确认输入生效"。
	FlashScreen(FLinearColor(0.50f, 0.68f, 0.95f, 1.f), 0.18f, 0.16f);

	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(BoundCharacter))
	{
		Player->ApplyCameraPunch(10.f, 0.18f);
	}
}

void UCombatHUDWidget::FlashScreen(FLinearColor Color, float Duration, float PeakOpacity)
{
	if (!ScreenFlash)
	{
		return;
	}

	FlashColor = Color;
	FlashDuration = FMath::Max(Duration, 0.02f);
	FlashRemaining = FlashDuration;
	FlashPeakOpacity = FMath::Clamp(PeakOpacity, 0.f, 1.f);

	ScreenFlash->SetVisibility(ESlateVisibility::HitTestInvisible);
	ScreenFlash->SetColorAndOpacity(FLinearColor(Color.R, Color.G, Color.B, FlashPeakOpacity));
}

void UCombatHUDWidget::ShowFeedbackText(const FText& InText, FLinearColor Color, float Duration)
{
	if (!FeedbackText)
	{
		return;
	}

	FeedbackText->SetText(InText);
	FeedbackText->SetColorAndOpacity(FSlateColor(Color));
	FeedbackText->SetVisibility(ESlateVisibility::HitTestInvisible);

	FeedbackTextRemaining = (Duration >= 0.f) ? Duration : FeedbackTextLifetime;
}

// ==================================================================================
// 每帧
// ==================================================================================
void UCombatHUDWidget::TickHUD(float InDeltaTime)
{
	if (FeedbackTextRemaining > 0.f)
	{
		FeedbackTextRemaining -= InDeltaTime;
		if (FeedbackTextRemaining <= 0.f && FeedbackText)
		{
			FeedbackText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	// 全屏闪光淡出。
	//
	// 用纯线性淡出而不是曲线：闪光本身只有 0.2~0.4s，
	// 曲线在这么短的时间里看不出来，反而多引入一个"手感不可预测"的变量。
	if (FlashRemaining > 0.f)
	{
		FlashRemaining = FMath::Max(0.f, FlashRemaining - InDeltaTime);

		if (ScreenFlash)
		{
			const float Alpha = (FlashRemaining / FlashDuration) * FlashPeakOpacity;
			ScreenFlash->SetColorAndOpacity(FLinearColor(FlashColor.R, FlashColor.G, FlashColor.B, Alpha));

			if (FlashRemaining <= 0.f)
			{
				// 必须收起来：留一张 alpha=0 的全屏 Image 虽然看不见，
				// 但它每帧仍在参与 Slate 的绘制遍历，属于纯浪费。
				ScreenFlash->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
	}

	// 数值轮询：读属性是纯内存读，但按 20Hz 同步就足够，
	// 让状态条的 Slate 不必每帧都重算一遍布局。
	SyncAccumulator += InDeltaTime;
	if (SyncAccumulator >= SyncInterval)
	{
		SyncAccumulator = 0.f;
		SyncFromCharacter();
	}

	// 锁定准星【不节流】：它要跟着角色和 BOSS 一起动，
	// 20Hz 会让它在快速走位时明显"一格一格地跳"。
	// 开销只有一次世界坐标投影 + 一次 SetPosition，可以每帧做。
	SyncLockOnMarker(InDeltaTime);
}

void UCombatHUDWidget::SyncLockOnMarker(float InDeltaTime)
{
	if (!LockOnMarkerRoot)
	{
		return;
	}

	// 从角色身上拿锁定组件，而不是 Cast 成 AFightPlayerCharacter：
	// 以后玩家角色换类（或者 BOSS 也能被锁），这里不用跟着改。
	ULockOnComponent* LockOnComp = BoundCharacter
		? BoundCharacter->FindComponentByClass<ULockOnComponent>()
		: nullptr;

	AActor* Target = (LockOnComp && LockOnComp->IsLockedOn()) ? LockOnComp->GetLockedTarget() : nullptr;

	if (!Target)
	{
		if (LockOnMarkerRoot->GetVisibility() != ESlateVisibility::Collapsed)
		{
			LockOnMarkerRoot->SetVisibility(ESlateVisibility::Collapsed);
		}
		LockOnMarkerAnim = -1.f;
		LastLockOnTarget = nullptr;
		return;
	}

	APlayerController* PC = GetOwningPlayer();
	if (!PC)
	{
		return;
	}

	FVector2D ScreenPos = FVector2D::ZeroVector;
	// 投影失败 = 目标在相机背后或视锥外 → 干脆隐藏，不做"贴边箭头"那套
	if (!PC->ProjectWorldLocationToScreen(LockOnComp->GetFocusLocation(), ScreenPos, /*bPlayerViewportRelative=*/false))
	{
		LockOnMarkerRoot->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	// ⚠⚠ 必须把【视口像素】换算成【控件本地单位】！（2026-09-26 根治"准星没锁在 BOSS 身上"）
	//
	// ProjectWorldLocationToScreen 返回的是视口像素；而 UCanvasPanelSlot 的 SetPosition
	// 收的是控件本地单位 —— 两者差一个 DPI / 视口缩放。
	// 本项目没配 DPI 曲线 → 走 UE 默认（ShortestSide），视口高 1080 时缩放 ≈ 0.76：
	// 不除这个缩放，准星的位置会被整体乘以 0.76，也就是**朝左上角拽**，
	// 表现就是"准星飘在 BOSS 左上方、看着没锁在身体中心"（相机其实是对的）。
	const float ViewportScale = UWidgetLayoutLibrary::GetViewportScale(this);
	if (ViewportScale > KINDA_SMALL_NUMBER)
	{
		ScreenPos /= ViewportScale;
	}

	// 换目标时重放入场动画：让"换锁"这件事被看见
	if (LastLockOnTarget.Get() != Target)
	{
		LastLockOnTarget = Target;
		LockOnMarkerAnim = 0.f;
	}

	// 入场：从 1.9 倍收到 1.0 倍（快速"咬住"目标），之后叠一层很轻的呼吸脉冲
	LockOnMarkerAnim = FMath::Max(0.f, LockOnMarkerAnim) + InDeltaTime;
	float Scale = 1.f;
	if (LockOnMarkerAnim < LockOnMarkerIntroTime)
	{
		const float Alpha = LockOnMarkerAnim / FMath::Max(LockOnMarkerIntroTime, KINDA_SMALL_NUMBER);
		// 前 30% 走完 70% 的收缩 → 起手很快、收尾很稳，比线性更有"咔哒"感
		const float Eased = 1.f - FMath::Pow(1.f - Alpha, 3.f);
		Scale = FMath::Lerp(1.9f, 1.f, Eased);
	}
	else
	{
		Scale = 1.f + FMath::Sin(LockOnMarkerAnim * 6.5f) * 0.035f;
	}

	LockOnMarkerRoot->SetRenderScale(FVector2D(Scale, Scale));

	if (UCanvasPanelSlot* MarkerSlot = Cast<UCanvasPanelSlot>(LockOnMarkerRoot->Slot))
	{
		MarkerSlot->SetPosition(ScreenPos);
	}

	if (LockOnMarkerRoot->GetVisibility() != ESlateVisibility::HitTestInvisible)
	{
		LockOnMarkerRoot->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void UCombatHUDWidget::SyncFromCharacter()
{
	if (!BoundCharacter)
	{
		return;
	}

	const float Health = BoundCharacter->GetHealth();
	const float MaxHealth = FMath::Max(BoundCharacter->GetMaxHealth(), 1.f);
	const float Stamina = BoundCharacter->GetStamina();
	const float MaxStamina = FMath::Max(BoundCharacter->GetMaxStamina(), 1.f);
	const float Poise = BoundCharacter->GetPoise();
	const float MaxPoise = FMath::Max(BoundCharacter->GetMaxPoise(), 1.f);

	if (HealthBar)
	{
		HealthBar->SetValueAndMax(Health, MaxHealth);
	}
	if (StaminaBar)
	{
		StaminaBar->SetValueAndMax(Stamina, MaxStamina);

		// 低体力 → 体力条在"正常绿 / 警告红"之间脉动。
		// 现在体力真的会限制"能打几刀、能闪多远"，光靠条变短玩家容易读成"还有余量"，
		// 必须换色才看得出来这是"该收手了"的预警。用世界时间做正弦，不额外起计时器。
		const FLinearColor StaminaGreen(0.20f, 0.68f, 0.30f, 1.f);
		const FLinearColor StaminaGhost(0.75f, 0.92f, 0.60f, 0.85f);

		if ((Stamina / MaxStamina) < DemoBalance::StaminaLowRatio)
		{
			const FLinearColor WarnRed(0.95f, 0.20f, 0.15f, 1.f);
			const float TimeSec = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
			const float Pulse = 0.5f + 0.5f * FMath::Sin(TimeSec * 12.f);
			StaminaBar->SetBarColors(StaminaGreen + (WarnRed - StaminaGreen) * Pulse, StaminaGhost);
		}
		else
		{
			StaminaBar->SetBarColors(StaminaGreen, StaminaGhost);
		}
	}
	if (PoiseBar)
	{
		PoiseBar->SetValueAndMax(Poise, MaxPoise);
	}
	if (PostureBar)
	{
		// 架势条读的是玩家"还能挡多久"。
		// 快见底时把整条染红 —— 这是唯一一条"越少越危险"的资源，
		// 不换色的话玩家会把"条短了"误读成"还有余量"。
		PostureBar->SetValueAndMax(BoundCharacter->GetPosture(),
			FMath::Max(BoundCharacter->GetMaxPosture(), 1.f));

		const bool bLow = BoundCharacter->GetPosture() <= FMath::Max(BoundCharacter->GetMaxPosture(), 1.f) * 0.3f;
		if (bLow != bLastPostureLow)
		{
			bLastPostureLow = bLow;
			PostureBar->SetBarColors(
				bLow ? FLinearColor(1.f, 0.25f, 0.18f, 1.f) : FLinearColor(0.98f, 0.78f, 0.22f, 1.f),
				bLow ? FLinearColor(1.f, 0.60f, 0.45f, 0.85f) : FLinearColor(1.f, 0.95f, 0.70f, 0.85f));
		}
	}

	// 回血"酒"持有量：只在数量或上限变化时重设文本，避免每帧 Invalidate Slate。
	if (HealItemText)
	{
		const AFightPlayerCharacter* PC = Cast<AFightPlayerCharacter>(BoundCharacter);
		const int32 CurCount = PC ? PC->GetHealItemCount() : 0;
		const int32 CurMax = PC ? PC->GetHealItemMaxCount() : 0;
		if (CurCount != LastHealItemCount || CurMax != LastHealItemMax)
		{
			LastHealItemCount = CurCount;
			LastHealItemMax = CurMax;
			HealItemText->SetText(FText::FromString(FString::Printf(TEXT("酒 ×%d/%d"), CurCount, CurMax)));
			// 喝光了就变灰，提示"该找补给 / 该省着用"
			const FLinearColor Color = (CurCount > 0)
				? FLinearColor(0.55f, 1.f, 0.7f, 1.f)
				: FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
			HealItemText->SetColorAndOpacity(FSlateColor(Color));
		}
	}

	// 只有真的变了才回调蓝图事件（否则每秒会触发 20 次音效/特效）
	if (!FMath::IsNearlyEqual(Health, LastHealthValue, 0.5f) || !FMath::IsNearlyEqual(MaxHealth, LastHealthMax, 0.5f))
	{
		LastHealthValue = Health;
		LastHealthMax = MaxHealth;
		OnHealthChanged(Health, MaxHealth);
	}
	if (!FMath::IsNearlyEqual(Stamina, LastStaminaValue, 0.5f) || !FMath::IsNearlyEqual(MaxStamina, LastStaminaMax, 0.5f))
	{
		LastStaminaValue = Stamina;
		LastStaminaMax = MaxStamina;
		OnStaminaChanged(Stamina, MaxStamina);
	}
	if (!FMath::IsNearlyEqual(Poise, LastPoiseValue, 0.5f) || !FMath::IsNearlyEqual(MaxPoise, LastPoiseMax, 0.5f))
	{
		LastPoiseValue = Poise;
		LastPoiseMax = MaxPoise;
		OnPoiseChanged(Poise, MaxPoise);
	}
}

// ==================================================================================
// BOSS 血条
// ==================================================================================
TSharedRef<SWidget> UBossHealthBarWidget::RebuildWidget()
{
	Initialize();

	if (bBuildDefaultLayout)
	{
		EnsureDefaultTree();
	}

	return Super::RebuildWidget();
}

void UBossHealthBarWidget::EnsureDefaultTree()
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

	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("BossBarCanvas"));
	if (!RootCanvas)
	{
		return;
	}
	RootCanvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// 阶段刻度线要在建树之前就定好（下面那个循环会按这个数组建线）
	if (bShowPhaseMarkers && PhaseMarkerRatios.Num() == 0)
	{
		PhaseMarkerRatios = { 0.55f };
	}

	USizeBox* Panel = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("BossPanel"));
	Panel->SetWidthOverride(BarWidth);

	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BossBox"));
	Panel->AddChild(Box);

	// ---- 名字 ----
	if (bShowName)
	{
		BossNameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BossNameText"));
		if (BossNameText)
		{
			ApplyFontSize(BossNameText, NameFontSize);
			ApplyOutline(BossNameText, FLinearColor(0.92f, 0.90f, 0.86f, 1.f), 1.f);
			BossNameText->SetText(FText::FromString(TEXT("BOSS")));

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(BossNameText))
			{
				VSlot->SetHorizontalAlignment(HAlign_Center);
				VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
			}
		}
	}

	// ---- 血条 + 阶段刻度叠在一起 ----
	{
		UOverlay* BarOverlay = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("BossBarOverlay"));
		if (BarOverlay)
		{
			BossHealthBar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), TEXT("BossHealthBar"));
			if (BossHealthBar)
			{
				BossHealthBar->BarHeight = 26.f;
				BossHealthBar->FillColor = Phase1FillColor;
				BossHealthBar->GhostColor = FLinearColor(0.97f, 0.90f, 0.62f, 0.92f);
				BossHealthBar->BackColor = FLinearColor(0.03f, 0.02f, 0.02f, 0.85f);
				BossHealthBar->ValueFontSize = 15;
				BossHealthBar->bShowValueText = true;

				BarOverlay->AddChildToOverlay(BossHealthBar);
				if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(BossHealthBar->Slot))
				{
					OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
					OverlaySlot->SetVerticalAlignment(VAlign_Fill);
				}
			}

			// 阶段刻度：一根细白线标在 55% 这类阈值上，让"两阶段"这件事在 UI 上看得见
			UCanvasPanel* MarkerCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("BossMarkerCanvas"));
			if (MarkerCanvas)
			{
				MarkerCanvas->SetVisibility(ESlateVisibility::HitTestInvisible);
				BarOverlay->AddChildToOverlay(MarkerCanvas);
				if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(MarkerCanvas->Slot))
				{
					OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
					OverlaySlot->SetVerticalAlignment(VAlign_Fill);
				}

				for (const float Ratio : PhaseMarkerRatios)
				{
					UImage* Marker = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
					if (!Marker)
					{
						continue;
					}

					Marker->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 0.65f));
					Marker->SetVisibility(ESlateVisibility::HitTestInvisible);

					if (UCanvasPanelSlot* MarkerSlot = MarkerCanvas->AddChildToCanvas(Marker))
					{
						// 只占一条竖线：锚点 X 收在 Ratio 这一点上，宽度靠 Offsets.Right 给 2px
						MarkerSlot->SetAnchors(FAnchors(FMath::Clamp(Ratio, 0.f, 1.f), 0.f,
							FMath::Clamp(Ratio, 0.f, 1.f), 1.f));
						MarkerSlot->SetAlignment(FVector2D(0.f, 0.f));
						MarkerSlot->SetAutoSize(false);
						MarkerSlot->SetOffsets(FMargin(0.f, 2.f, 2.f, -2.f));
					}

					PhaseMarkers.Add(Marker);
				}
			}

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(BarOverlay))
			{
				VSlot->SetHorizontalAlignment(HAlign_Fill);
			}
		}
	}

	// ---- 双异常条（冲击 橙 / 元素 青）----
	//
	// 位置刻意放在【血条正下方、韧性条之上】：参考 Demo 就是把两条异常槽直接挂在 BOSS 血条下面，
	// 玩家看血量的时候顺带就看到进度，视线不用在屏幕两侧来回跳。
	//
	// 两根条各带一个固定标签（冲击 / 元素），是为了回答"这根条是谁喂的"：
	// 冲击条由普攻连段推进，元素条由弹反推进 —— 玩家看到"元素条不动"就知道自己该去弹一刀了，
	// 这就是耦合设计在 UI 上的可读性。
	if (bShowAnomalyBars)
	{
		AnomalyImpactBar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), TEXT("BossAnomalyImpactBar"));
		if (AnomalyImpactBar)
		{
			AnomalyImpactBar->BarHeight = 9.f;
			AnomalyImpactBar->SetBarColors(
				FLinearColor(1.f, 0.55f, 0.12f, 1.f),      // 橙：冲击
				FLinearColor(1.f, 0.80f, 0.45f, 0.30f));
			AnomalyImpactBar->BackColor = FLinearColor(0.03f, 0.02f, 0.01f, 0.80f);
			AnomalyImpactBar->ValueFontSize = 10;
			AnomalyImpactBar->SetShowValueText(false);
			AnomalyImpactBar->SetLabel(FText::FromString(TEXT("冲击 · 失衡")));
			AnomalyImpactBar->SetPercent(0.f, true);

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(AnomalyImpactBar))
			{
				VSlot->SetHorizontalAlignment(HAlign_Fill);
				VSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
			}
		}

		AnomalyElementBar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), TEXT("BossAnomalyElementBar"));
		if (AnomalyElementBar)
		{
			AnomalyElementBar->BarHeight = 9.f;
			AnomalyElementBar->SetBarColors(
				FLinearColor(0.25f, 0.90f, 0.95f, 1.f),    // 青：元素
				FLinearColor(0.60f, 0.95f, 1.f, 0.30f));
			AnomalyElementBar->BackColor = FLinearColor(0.01f, 0.03f, 0.04f, 0.80f);
			AnomalyElementBar->ValueFontSize = 10;
			AnomalyElementBar->SetShowValueText(false);
			AnomalyElementBar->SetLabel(FText::FromString(TEXT("元素 · 击破")));
			AnomalyElementBar->SetPercent(0.f, true);

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(AnomalyElementBar))
			{
				VSlot->SetHorizontalAlignment(HAlign_Fill);
				VSlot->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
			}
		}

		// 状态浮动标签：UNSTABLE / BROKEN。
		// 和 BOSS 的材质轮廓是同一件事的两条通道 —— 玩家盯着 BOSS 时看轮廓，
		// 看向 HUD 时看这个标签；只做一条的话，总有一半时间是"状态发生了但我没看见"。
		AnomalyStateText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BossAnomalyStateText"));
		if (AnomalyStateText)
		{
			ApplyFontSize(AnomalyStateText, 16);
			ApplyOutline(AnomalyStateText, FLinearColor(0.f, 0.f, 0.f, 1.f), 2.f);
			AnomalyStateText->SetText(FText::FromString(TEXT("UNSTABLE")));
			AnomalyStateText->SetVisibility(ESlateVisibility::Collapsed);

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(AnomalyStateText))
			{
				VSlot->SetHorizontalAlignment(HAlign_Center);
				VSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
			}
		}
	}

	// ---- 韧性条 ----
	if (bShowPoiseBar)
	{
		BossPoiseBar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), TEXT("BossPoiseBar"));
		if (BossPoiseBar)
		{
			BossPoiseBar->BarHeight = 10.f;
			BossPoiseBar->FillColor = FLinearColor(0.45f, 0.55f, 0.90f, 1.f);
			BossPoiseBar->GhostColor = FLinearColor(0.85f, 0.85f, 0.95f, 0.8f);
			BossPoiseBar->BackColor = FLinearColor(0.02f, 0.02f, 0.04f, 0.8f);
			BossPoiseBar->ValueFontSize = 10;
			BossPoiseBar->SetShowValueText(false);

			if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(BossPoiseBar))
			{
				VSlot->SetHorizontalAlignment(HAlign_Fill);
				VSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
			}
		}
	}

	// ---- 阶段文字 ----
	BossPhaseText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BossPhaseText"));
	if (BossPhaseText)
	{
		ApplyFontSize(BossPhaseText, 13);
		ApplyOutline(BossPhaseText, FLinearColor(0.86f, 0.84f, 0.80f, 1.f), 1.f);
		BossPhaseText->SetText(FText::GetEmpty());

		if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(BossPhaseText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
		}
	}

	// ---- 起手预警条（默认隐藏）----
	//
	// 这是"玩家能不能打出弹反"的关键 UI：BOSS 每次出招前都会先进入起手预警，
	// 这段时间里显示一条从 0 填满到 1 的进度条 —— 填满的那一刻就是出招瞬间，
	// 也就是玩家该按右键（弹刀）或 Shift（闪避）的时机。
	//
	// 颜色即规则（由 Brain::CanCurrentAttackBeParried 决定）：
	//   金色 = 这招可以弹反 → 按右键
	//   红色 = 这招弹不了   → 只能闪避
	//
	// 没有这条 UI 的话，弹反只能靠肉眼读动画猜时机，手感自然是"玄学"而不是"技术"。
	// 有了它，玩家的每一次成功弹反都能归因到"我在条填满的瞬间按了键"，
	// 学习曲线才成立 —— 这是"交互感"里最实在的一块。
	TelegraphBar = WidgetTree->ConstructWidget<UDemoStatusBarWidget>(UDemoStatusBarWidget::StaticClass(), TEXT("BossTelegraphBar"));
	if (TelegraphBar)
	{
		TelegraphBar->BarHeight = 13.f;
		TelegraphBar->BackColor = FLinearColor(0.02f, 0.02f, 0.03f, 0.80f);
		TelegraphBar->ValueFontSize = 13;
		TelegraphBar->bShowValueText = true;
		TelegraphBar->bAppendMaxValue = false;
		// 延迟掉血层对"倒计时"没有意义，把它和填充设成同色，视觉上就是一条干净的进度条
		TelegraphBar->SetBarColors(
			FLinearColor(1.f, 0.80f, 0.24f, 1.f),
			FLinearColor(1.f, 0.80f, 0.24f, 0.30f));
		TelegraphBar->SetLabel(FText::FromString(TEXT("起手预警")));
		TelegraphBar->SetPercent(0.f, true);
		TelegraphBar->SetVisibility(ESlateVisibility::Collapsed);

		if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(TelegraphBar))
		{
			VSlot->SetHorizontalAlignment(HAlign_Fill);
			VSlot->SetPadding(FMargin(0.f, 5.f, 0.f, 0.f));
		}
	}

	// ---- 大招提示（默认隐藏）----
	BossHintText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BossHintText"));
	if (BossHintText)
	{
		ApplyFontSize(BossHintText, 18);
		ApplyOutline(BossHintText, FLinearColor(1.f, 0.42f, 0.30f, 1.f), 2.f);
		BossHintText->SetText(FText::FromString(TEXT("!! 大招蓄力中 —— 快跑位 !!")));
		BossHintText->SetVisibility(ESlateVisibility::Collapsed);

		if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(BossHintText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
		}
	}

	// ---- "已造成伤害"读数 ----
	//
	// BOSS 血量 10000、玩家单段普攻 28~56 → 一刀只占血条的 0.3%~0.6%，
	// 血条长度上就是 2~4 像素，肉眼看不出来，很容易误判成"没扣血"。
	// 这个累计数字 + 百分比才是"确实在扣血"的直接证据。
	DamageDealtText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BossDamageDealtText"));
	if (DamageDealtText)
	{
		ApplyFontSize(DamageDealtText, 14);
		ApplyOutline(DamageDealtText, FLinearColor(0.f, 0.f, 0.f, 1.f), 1.f);
		DamageDealtText->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.92f, 1.f, 0.95f)));
		DamageDealtText->SetText(FText::FromString(TEXT("已造成伤害 0（0.0%）")));

		if (UVerticalBoxSlot* VSlot = Box->AddChildToVerticalBox(DamageDealtText))
		{
			VSlot->SetHorizontalAlignment(HAlign_Center);
			VSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
		}
	}

	if (UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(Panel))
	{
		PanelSlot->SetAnchors(FAnchors(0.5f, 0.f));   // 顶部居中
		PanelSlot->SetAlignment(FVector2D(0.5f, 0.f));
		PanelSlot->SetAutoSize(true);
		PanelSlot->SetPosition(FVector2D(0.f, TopOffset));
	}

	// ---- 伤害飘字层（BOSS 挨打的数字画在这里）----
	DamageNumberLayer = WidgetTree->ConstructWidget<UDemoDamageNumberLayer>(UDemoDamageNumberLayer::StaticClass(), TEXT("BossDamageNumberLayer"));
	if (DamageNumberLayer)
	{
		if (UCanvasPanelSlot* LayerSlot = RootCanvas->AddChildToCanvas(DamageNumberLayer))
		{
			// 铺满全屏（飘字用的是屏幕绝对坐标）
			LayerSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			LayerSlot->SetOffsets(FMargin(0.f));
		}
	}

	WidgetTree->RootWidget = RootCanvas;
	bTreeReady = true;
}
void UBossHealthBarWidget::BuildPhaseMarkers()
{
	// PhaseMarkers 已经跟着 PhaseMarkerRatios 建好了；这个函数留给蓝图重构建时调用
}

// ==================================================================================
// 绑定
// ==================================================================================
void UBossHealthBarWidget::BindToBoss(ABossCharacterBase* Boss)
{
	BoundBoss = Boss;

	if (!Boss)
	{
		return;
	}

	Boss->OnCharacterDeath.AddDynamic(this, &UBossHealthBarWidget::HandleBossDeath);
	Boss->OnDamagedDetailed.AddDynamic(this, &UBossHealthBarWidget::HandleBossDamagedDetailed);

	if (UBossBrainComponent* Brain = Boss->GetBrain())
	{
		Brain->OnPhaseChanged.AddDynamic(this, &UBossHealthBarWidget::HandleBossPhaseChanged);
	}

	if (BossNameText)
	{
		const FText DisplayName = Boss->DisplayName.IsEmpty()
			? FText::FromString(Boss->GetName())
			: Boss->DisplayName;
		BossNameText->SetText(DisplayName);
	}

	// 换绑目标 / 重开一局时把累计伤害清零，否则会接着上一只 BOSS 的数字往上加
	AccumulatedDamage = 0.f;
	UpdateDamageDealtText();

	SyncAccumulator = SyncInterval;
	SyncFromBoss();
}

void UBossHealthBarWidget::HandleBossDeath()
{
	if (!bDeathShown)
	{
		bDeathShown = true;

		if (BossHintText)
		{
			BossHintText->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (BossPhaseText)
		{
			BossPhaseText->SetText(FText::FromString(TEXT("—— 已 击 破 ——")));
		}
	}

	OnBossDeath();
}

void UBossHealthBarWidget::HandleBossDamagedDetailed(float Damage, EDemoHitResultType ResultType,
	EDemoDamageSource DamageSource, FVector HitDirection)
{
	if (DamageNumberLayer && BoundBoss)
	{
		// 飘字挂在"受击点上方"，看起来才像打在这只怪身上
		const FVector Where = BoundBoss->GetActorLocation() + FVector(0.f, 0.f, 190.f);
		DamageNumberLayer->SpawnDamageNumber(Where, Damage, ResultType, DamageSource, false);
	}

	// 累计伤害读数：这是"打中了到底有没有掉血"最可靠的证据
	//（血条一刀只动 2~4 像素，看不出变化）
	AccumulatedDamage += FMath::Max(Damage, 0.f);
	UpdateDamageDealtText();

	OnBossDamagedDetailed(Damage, ResultType, DamageSource, HitDirection);
}

void UBossHealthBarWidget::UpdateDamageDealtText()
{
	if (!DamageDealtText)
	{
		return;
	}

	const float MaxHealth = BoundBoss ? FMath::Max(BoundBoss->GetMaxHealth(), 1.f) : 1.f;
	const float Percent = AccumulatedDamage / MaxHealth * 100.f;

	DamageDealtText->SetText(FText::FromString(FString::Printf(
		TEXT("已造成伤害 %d（%.1f%%）"), FMath::RoundToInt(AccumulatedDamage), Percent)));
}

void UBossHealthBarWidget::HandleBossPhaseChanged(int32 PhaseIndex)
{
	LastPhaseIndex = PhaseIndex;

	const bool bPhase2 = PhaseIndex > 0;

	if (BossHealthBar)
	{
		BossHealthBar->SetBarColors(
			bPhase2 ? Phase2FillColor : Phase1FillColor,
			FLinearColor(0.97f, 0.90f, 0.62f, 0.92f));
	}

	if (BossPhaseText)
	{
		BossPhaseText->SetText(bPhase2
			? FText::FromString(TEXT("第二阶段 · 狂化"))
			: FText::FromString(TEXT("第一阶段")));
	}

	OnBossPhaseChanged(PhaseIndex);
}

// ==================================================================================
// 每帧
// ==================================================================================
void UBossHealthBarWidget::TickHUD(float InDeltaTime)
{
	// 预警条不节流：起手窗只有 0.5~0.85s，20Hz 采样会让进度条一顿一顿的，
	// 而"条填满的瞬间"正是玩家要按键的时机。详见 SyncTelegraph 的注释。
	SyncTelegraph();

	// 状态标签每帧查：失衡/击破是"跃迁"，晚一帧玩家就觉得提示跟不上动作
	SyncAnomalyStateLabel();

	SyncAccumulator += InDeltaTime;
	if (SyncAccumulator >= SyncInterval)
	{
		SyncAccumulator = 0.f;
		SyncFromBoss();
	}
}

void UBossHealthBarWidget::SyncTelegraph()
{
	if (!TelegraphBar)
	{
		return;
	}

	UBossBrainComponent* Brain = BoundBoss ? BoundBoss->GetBrain() : nullptr;

	// 不在起手状态 → 收起预警条
	if (!Brain || !Brain->IsTelegraphing())
	{
		TelegraphBar->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	const bool bCanParry = Brain->CanCurrentAttackBeParried();

	// ---- 颜色即规则：金 = 可以弹，红 = 只能闪 ----
	// 只在颜色真的变了才写 Brush：SetBarColors 会 Invalidate Slate，
	// 每帧无条件调用等于让这条进度条每帧都重绘一次。
	if (bCanParry != bLastTelegraphParriable)
	{
		bLastTelegraphParriable = bCanParry;

		const FLinearColor Fill = bCanParry
			? FLinearColor(1.f, 0.80f, 0.24f, 1.f)     // 金：可弹反
			: FLinearColor(0.95f, 0.26f, 0.20f, 1.f);   // 红：不可弹反
		const FLinearColor Ghost = Fill.CopyWithNewOpacity(0.30f);
		TelegraphBar->SetBarColors(Fill, Ghost);

		// 文字刻意不带倒计时数字：
		//   ① 剩余时间用进度条长度表达更直观（玩家看的是"还差多少填满"）；
		//   ② 每帧变化的数字会逼着 Slate 每帧重建文本，纯浪费。
		TelegraphBar->SetLabel(bCanParry
			? FText::FromString(TEXT("可弹反 —— 按右键"))
			: FText::FromString(TEXT("弹不了 —— 按 Shift 闪避")));
	}

	// 进度 = 出招倒计时。填满的瞬间 BOSS 就出手，也就是该按键的瞬间。
	// bInstant=true：读招条不需要"延迟残影"，那个效果在这里反而会让人误判时机。
	TelegraphBar->SetPercent(Brain->GetTelegraphProgress(), true);
	TelegraphBar->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UBossHealthBarWidget::SyncAnomalyBars()
{
	if (!BoundBoss)
	{
		return;
	}

	if (AnomalyImpactBar)
	{
		const float Max = FMath::Max(BoundBoss->GetMaxAnomalyImpact(), 1.f);
		AnomalyImpactBar->SetValueAndMax(BoundBoss->GetAnomalyImpact(), Max);
	}

	if (AnomalyElementBar)
	{
		const float Max = FMath::Max(BoundBoss->GetMaxAnomalyElement(), 1.f);
		AnomalyElementBar->SetValueAndMax(BoundBoss->GetAnomalyElement(), Max);
	}
}

void UBossHealthBarWidget::SyncAnomalyStateLabel()
{
	if (!AnomalyStateText || !BoundBoss)
	{
		return;
	}

	const UAbilitySystemComponent* ASC = BoundBoss->GetAbilitySystemComponent();
	const bool bBroken = ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Broken);
	const bool bUnstable = ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Unstable);

	if (!bBroken && !bUnstable)
	{
		if (bLastBrokenShown || bLastUnstableShown)
		{
			AnomalyStateText->SetVisibility(ESlateVisibility::Collapsed);
			bLastBrokenShown = false;
			bLastUnstableShown = false;
		}
		return;
	}

	// 只在跃迁时改文字与颜色：SetColorAndOpacity 会让 Slate 重绘，
	// 每帧无条件写等于给这条标签开了个每帧 Invalidate 的口子。
	if (bBroken != bLastBrokenShown || bUnstable != bLastUnstableShown)
	{
		bLastBrokenShown = bBroken;
		bLastUnstableShown = bUnstable;
		AnomalyStatePulse = 0.f;

		AnomalyStateText->SetText(bBroken
			? FText::FromString(TEXT("BROKEN —— 失去格挡"))
			: FText::FromString(TEXT("UNSTABLE —— 失衡")));

		AnomalyStateText->SetColorAndOpacity(bBroken
			? FSlateColor(FLinearColor(1.f, 0.42f, 0.18f, 1.f))   // 击破：橙红
			: FSlateColor(FLinearColor(0.35f, 1.f, 0.55f, 1.f))); // 失衡：绿（和 BOSS 轮廓同色，两条通道互相印证）

		AnomalyStateText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void UBossHealthBarWidget::SyncFromBoss()
{
	if (!BoundBoss)
	{
		return;
	}

	// 双异常条（20Hz 与血/韧同一节拍，慢变量不需要每帧刷）
	SyncAnomalyBars();

	const float Health = BoundBoss->GetHealth();
	const float MaxHealth = FMath::Max(BoundBoss->GetMaxHealth(), 1.f);
	const float Poise = BoundBoss->GetPoise();
	const float MaxPoise = FMath::Max(BoundBoss->GetMaxPoise(), 1.f);

	if (BossHealthBar)
	{
		BossHealthBar->SetValueAndMax(Health, MaxHealth);
	}
	if (BossPoiseBar)
	{
		BossPoiseBar->SetValueAndMax(Poise, MaxPoise);
	}

	if (!FMath::IsNearlyEqual(Health, LastBossHealth, 0.5f) || !FMath::IsNearlyEqual(MaxHealth, LastBossHealthMax, 0.5f))
	{
		LastBossHealth = Health;
		LastBossHealthMax = MaxHealth;
		OnBossHealthChanged(Health, MaxHealth);
	}
	if (!FMath::IsNearlyEqual(Poise, LastBossPoise, 0.5f) || !FMath::IsNearlyEqual(MaxPoise, LastBossPoiseMax, 0.5f))
	{
		LastBossPoise = Poise;
		LastBossPoiseMax = MaxPoise;
		OnBossPoiseChanged(Poise, MaxPoise);
	}

	// ---- 大招蓄力提示 ----
	// 不走事件而是每帧问一次：蓄力是个"持续状态"，用事件要在开始/结束各发一次，
	// 漏一个就会卡在常亮的提示上。
	if (BossHintText)
	{
		bool bCharging = false;

		if (UBossBrainComponent* Brain = BoundBoss->GetBrain())
		{
			if (Brain->GetState() == EBossState::Attack)
			{
				if (const UAbilitySystemComponent* ASC = BoundBoss->GetAbilitySystemComponent())
				{
					bCharging = ASC->HasMatchingGameplayTag(DemoTag::State_Boss_Charging);
				}
			}
		}

		BossHintText->SetVisibility(bCharging ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}
