// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class demo : ModuleRules
{
	public demo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore",
			"EnhancedInput",
			"GameplayAbilities", "GameplayTags", "GameplayTasks",
			"AIModule", "NavigationSystem",
			"UMG", "Slate", "SlateCore", "Niagara",
			// UE 5.7 的 ULegacyCameraShake（命中 Screen Shake 用）在 EngineCameras 插件里
			"EngineCameras"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// 源码按功能分子目录，需要把模块根目录加入 include 路径
		PublicIncludePaths.Add(ModuleDirectory);
		// 子目录也加入（用绝对路径：写成相对字符串会被 UBT 解析到 Source/ 下，
		// 并报一堆 "Referenced directory ... does not exist" 警告）
		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(ModuleDirectory, "Core"),
			Path.Combine(ModuleDirectory, "Abilities"),
			Path.Combine(ModuleDirectory, "Animation"),
			Path.Combine(ModuleDirectory, "Boss"),
			Path.Combine(ModuleDirectory, "Combat"),
			Path.Combine(ModuleDirectory, "Effects"),
			Path.Combine(ModuleDirectory, "Game"),
			Path.Combine(ModuleDirectory, "Player"),
			Path.Combine(ModuleDirectory, "UI")
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
