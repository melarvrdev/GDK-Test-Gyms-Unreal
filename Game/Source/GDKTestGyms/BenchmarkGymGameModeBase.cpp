// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "BenchmarkGymGameModeBase.h"

#include "Engine/World.h"
#include "EngineClasses/SpatialActorChannel.h"
#include "EngineClasses/SpatialNetDriver.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/MovementComponent.h"
#include "GDKTestGymsGameInstance.h"
#include "GeneralProjectSettings.h"
#include "Interop/Connection/SpatialWorkerConnection.h"
#include "Interop/SpatialWorkerFlags.h"
#include "Kismet/GameplayStatics.h"
#include "LoadBalancing/SpatialMultiWorkerSettings.h"
#include "LoadBalancing/GridBasedLBStrategy.h"
#include "Misc/CommandLine.h"
#include "Net/UnrealNetwork.h"
#include "SpatialConstants.h"
#include "SpatialView/EntityView.h"
#include "TimerManager.h"
#include "UserExperienceComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Utils/SpatialMetrics.h"
#include "Utils/SpatialStatics.h"

DEFINE_LOG_CATEGORY(LogBenchmarkGymGameModeBase);

// Metrics
namespace
{
	const FString AverageClientRTTMetricName = TEXT("UnrealAverageClientRTT");
	const FString AverageClientUpdateTimeDeltaMetricName = TEXT("UnrealAverageClientUpdateTimeDelta");
	const FString ExpectedPlayersValidMetricName = TEXT("ExpectedPlayersValid");
	const FString AverageFPSValid = TEXT("UnrealServerFPSValid");
	const FString AverageClientFPSValid = TEXT("UnrealClientFPSValid");
	const FString ActorCountValidMetricName = TEXT("UnrealActorCountValid");
	const FString PlayerMovementMetricName = TEXT("UnrealPlayerMovement");

	const FString MaxRoundTripWorkerFlag = TEXT("max_round_trip");
	const FString MaxUpdateTimeDeltaWorkerFlag = TEXT("max_update_time_delta");
	const FString MaxRoundTripCommandLineKey = TEXT("-MaxRoundTrip=");
	const FString MaxUpdateTimeDeltaCommandLineKey = TEXT("-MaxUpdateTimeDelta=");

	const FString TestLiftimeWorkerFlag = TEXT("test_lifetime");
	const FString TestLiftimeCommandLineKey = TEXT("-TestLifetime=");

	const FString TotalPlayerWorkerFlag = TEXT("total_players");
	const FString TotalNPCsWorkerFlag = TEXT("total_npcs");
	const FString RequiredPlayersWorkerFlag = TEXT("required_players");
	const FString TotalPlayerCommandLineKey = TEXT("-TotalPlayers=");
	const FString TotalNPCsCommandLineKey = TEXT("-TotalNPCs=");
	const FString RequiredPlayersCommandLineKey = TEXT("-RequiredPlayers=");

	const FString CubeRespawnBaseTimeWorkerFlag = TEXT("cube_base_respawn_time");
	const FString CubeRespawnBaseTimeCommandLineKey = TEXT("-CubeBaseRespawnTime=");

	const FString CubeRespawnRandomRangeTimeWorkerFlag = TEXT("cube_random_range_respawn_time");
	const FString CubeRespawnRandomRangeCommandLineKey = TEXT("-CubeRandomRangeRespawnTime=");

#if	STATS
	const FString StatProfileWorkerFlag = TEXT("stat_profile");
	const FString StatProfileCommandLineKey = TEXT("-StatProfile=");
#endif
#if !UE_BUILD_SHIPPING
	const FString MemReportFlag = TEXT("mem_report");
	const FString MemRemportIntervalKey = TEXT("-MemReportInterval=");
#endif
	const FString MetricLeftLabel = TEXT("metric");
	const FString MetricName = TEXT("improbable_engine_metrics");
	const FString MetricEnginePlatformLeftLabel = TEXT("engine_platform");
	const FString MetricEnginePlatformRightLabel = TEXT("UnrealWorker");

	const bool bEnableDensityBucketOutput = false;

} // anonymous namespace

FString ABenchmarkGymGameModeBase::ReadFromCommandLineKey = TEXT("ReadFromCommandLine");

ABenchmarkGymGameModeBase::ABenchmarkGymGameModeBase()
	: ExpectedPlayers(0) // ExpectedPlayers is invalid until set via command line arg or worker flag.
	, RequiredPlayers(4096)
	, TotalNPCs(0) // TotalNPCs is invalid until set via command line arg or worker flag.
	, bLongFormScenario(false)
	, NumWorkers(1)
	, ZoningCols(1)
	, ZoningRows(1)
	, ZoneWidth(1000000.0f)
	, ZoneHeight(1000000.0f)
	, AveragedClientRTTMS(0.0)
	, AveragedClientUpdateTimeDeltaMS(0.0)
	, MaxClientRoundTripMS(150)
	, MaxClientUpdateTimeDeltaMS(300)
	, bHasUxFailed(false)
	, bHasFpsFailed(false)
	, bHasClientFpsFailed(false)
	, bHasActorCountFailed(false)
	, bActorCountFailureState(false)
	, UXAuthActorCount(0)
	, PrintMetricsTimer(10)
	, TestLifetimeTimer(0)
	, TimeSinceLastCheckedTotalActorCounts(0.0f)
	, bHasRequiredPlayersCheckFailed(false)
	, RequiredPlayerCheckTimer(11*60) // all clients should have joined by this point (seconds)
	, DeploymentValidTimer(16*60) // time to finish RequiredPlayerCheckTimer, to allow workers to disconnect without failing test (seconds)
	, CurrentPlayerAvgVelocity(0.0f)
	, RecentPlayerAvgVelocity(0.0f)
	, RequiredPlayerMovementReportTimer(5 * 60)
	, RequiredPlayerMovementCheckTimer(6 * 60)
	, CubeRespawnBaseTime(10.0f)
	, CubeRespawnRandomRangeTime(10.0f)
#if	STATS
	, StatStartFileTimer(60 * 60 * 24)
	, StatStopFileTimer(60)
	, MemReportIntervalTimer(60 * 60 * 24)
#endif
{
	PrimaryActorTick.bCanEverTick = true;

	if (USpatialStatics::IsSpatialNetworkingEnabled())
	{
		bAlwaysRelevant = true;
	}
}

void ABenchmarkGymGameModeBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABenchmarkGymGameModeBase, TotalNPCs);
	DOREPLIFETIME(ABenchmarkGymGameModeBase, ActorCountReportIdx);
}

void ABenchmarkGymGameModeBase::BeginPlay()
{
	Super::BeginPlay();

	GatherWorkerConfiguration();
	ParsePassedValues();
	TryBindWorkerFlagsDelegates();
	TryAddSpatialMetrics();

	InitialiseActorCountCheckTimer();

	if (bEnableDensityBucketOutput && GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
	{
		OutputPlayerDensity();
	}

	if (bLongFormScenario)
	{
		// Extend timers to handle longer expected deployment lifetime (required for current long form disco performance test)
		RequiredPlayerCheckTimer.SetTimer(17 * 60);
		DeploymentValidTimer.SetTimer(38 * 60);
		UNFRConstants* NFRConstants = const_cast<UNFRConstants*>(UNFRConstants::Get(GetWorld()));
		NFRConstants->ActorCheckDelay.SetTimer(16*60);
	}
}

void ABenchmarkGymGameModeBase::OnAuthorityLost()
{
	Super::OnAuthorityLost();

	ensureMsgf(false, TEXT("ABenchmarkGymGameModeBase doesn't support authority transfer"));
}

void ABenchmarkGymGameModeBase::InitialiseActorCountCheckTimer()
{
	FTimerManager& TimerManager = GetWorld()->GetTimerManager();

	// Timer to build expected actor counts using worker flags or CMD argument after a delay.
	FTimerHandle InitialiseExpectedActorCountsTimerHandle;
	const float InitialiseExpectedActorCountsDelayInSeconds = 30.0f;
	TimerManager.SetTimer(
		InitialiseExpectedActorCountsTimerHandle,
		[WeakThis = TWeakObjectPtr<ABenchmarkGymGameModeBase>(this)]() {
			if (ABenchmarkGymGameModeBase* GameMode = WeakThis.Get())
			{
				GameMode->BuildExpectedActorCounts();
			}
		},
		InitialiseExpectedActorCountsDelayInSeconds, false);

	if (HasAuthority())
	{
		// Timer trigger periodic check of total actor count across all workers.
		TimerManager.SetTimer(
			UpdateActorCountCheckTimerHandle,
			[WeakThis = TWeakObjectPtr<ABenchmarkGymGameModeBase>(this)]() {
				if (ABenchmarkGymGameModeBase* GameMode = WeakThis.Get())
				{
					GameMode->UpdateActorCountCheck();
				}
			},
			UpdateActorCountCheckPeriodInSeconds, true,
			UpdateActorCountCheckInitialDelayInSeconds);
	}
}

void ABenchmarkGymGameModeBase::GatherWorkerConfiguration()
{
	// No need to fiddle with configuration as the defaults should reflect the single server scenario which is all that's required in native.
	if (!USpatialStatics::IsSpatialNetworkingEnabled())
	{
		return;
	}

	const UWorld* World = GetWorld();
	const UAbstractSpatialMultiWorkerSettings* MultiWorkerSettings =
		USpatialStatics::GetSpatialMultiWorkerClass(World)->GetDefaultObject<UAbstractSpatialMultiWorkerSettings>();

	if (MultiWorkerSettings != nullptr && MultiWorkerSettings->WorkerLayers.Num() > 0)
	{
		NumWorkers = MultiWorkerSettings->GetMinimumRequiredWorkerCount();

		const UAbstractLBStrategy* LBStrategy = GetDefault<UAbstractLBStrategy>(MultiWorkerSettings->WorkerLayers[0].LoadBalanceStrategy);
		const UGridBasedLBStrategy* GridLBStrategy = Cast<UGridBasedLBStrategy>(LBStrategy);
		if (GridLBStrategy != nullptr)
		{
			ZoningRows = FMath::Max(1, static_cast<int32>(GridLBStrategy->GetRows()));
			ZoningCols = FMath::Max(1, static_cast<int32>(GridLBStrategy->GetCols()));
			ZoneWidth = GridLBStrategy->GetWorldWidth() / ZoningCols;
			ZoneHeight = GridLBStrategy->GetWorldHeight() / ZoningRows;
		}
	}
}

void ABenchmarkGymGameModeBase::BuildExpectedActorCounts()
{
	// Zoning scenarios can report actor count numbers slightly higher than the expected number so add a little slack.
	// This is due to the fact that server report their auth actor counts out of sync.
	AddExpectedActorCount(NPCClass, TotalNPCs - 1, FMath::CeilToInt(TotalNPCs * 1.05));
	AddExpectedActorCount(SimulatedPawnClass, RequiredPlayers, FMath::CeilToInt(ExpectedPlayers * 1.05));
}

void ABenchmarkGymGameModeBase::UpdateActorCountCheck()
{
	if (HasAuthority())
	{
		ActorCountReportIdx++;
		UpdateAndReportActorCounts();

		FTimerManager& TimerManager = GetWorld()->GetTimerManager();
		if (!TimerManager.IsTimerActive(FailActorCountTimeoutTimerHandle))
		{
			const float FailActorCountTimeout = 2.5f * UpdateActorCountCheckPeriodInSeconds;
			TimerManager.SetTimer(
				FailActorCountTimeoutTimerHandle,
				[WeakThis = TWeakObjectPtr<ABenchmarkGymGameModeBase>(this)]() {
					if (ABenchmarkGymGameModeBase* GameMode = WeakThis.Get())
					{
						GameMode->FailActorCountDueToTimeout();
					}
				},
				FailActorCountTimeout, false);
			GetMetrics(MetricLeftLabel, ActorCountValidMetricName, MetricName, &ABenchmarkGymGameModeBase::GetActorCountValid);
		}
	}
}

void ABenchmarkGymGameModeBase::FailActorCountDueToTimeout()
{
	bActorCountFailureState = true;
	if (!bHasActorCountFailed)
	{
		bHasActorCountFailed = true;
		NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: Actor count was not checked at reasonable frequency."), *NFRFailureString);
	}
}

void ABenchmarkGymGameModeBase::AddExpectedActorCount(const TSubclassOf<AActor>& ActorClass, const int32 MinCount, const int32 MaxCount)
{
	if (ActorClass == nullptr)
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("Null class passed in to AddExpectedActorCount"));
		return;
	}

	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Adding NFR actor count expectation - ActorClass: %s, MinCount: %d, MaxCount: %d"), *ActorClass->GetName(), MinCount, MaxCount);
	ExpectedActorCounts.Add(ActorClass, FExpectedActorCountConfig(MinCount, MaxCount));
}

void ABenchmarkGymGameModeBase::TryBindWorkerFlagsDelegates()
{
	if (!GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
	{
		return;
	}

	const FString& CommandLine = FCommandLine::Get();
	if (FParse::Param(*CommandLine, *ReadFromCommandLineKey))
	{
		return;
	}

	USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GetNetDriver());
	if (ensure(SpatialDriver != nullptr))
	{
		USpatialWorkerFlags* SpatialWorkerFlags = SpatialDriver->SpatialWorkerFlags;
		if (ensure(SpatialWorkerFlags != nullptr))
		{
			BindWorkerFlagDelegates(SpatialWorkerFlags);
		}
	}
}

void ABenchmarkGymGameModeBase::BindWorkerFlagDelegates(USpatialWorkerFlags* SpatialWorkerFlags)
{
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnExpectedPlayersFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(TotalPlayerWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnRequiredPlayersFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(RequiredPlayersWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnTotalNPCsFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(TotalNPCsWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnMaxRoundTripFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(MaxRoundTripWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnMaxUpdateTimeDeltaFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(MaxUpdateTimeDeltaWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnTestLiftimeFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(TestLiftimeWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnCubeRespawnBaseTimeFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(CubeRespawnBaseTimeWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnCubeRespawnRandomRangeTimeUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(CubeRespawnRandomRangeTimeWorkerFlag, WorkerFlagDelegate);
	}
#if	STATS
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnStatProfileFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(StatProfileWorkerFlag, WorkerFlagDelegate);
	}
	{
		FOnWorkerFlagUpdatedBP WorkerFlagDelegate;
		WorkerFlagDelegate.BindDynamic(this, &ABenchmarkGymGameModeBase::OnMemReportFlagUpdate);
		SpatialWorkerFlags->RegisterFlagUpdatedCallback(MemReportFlag, WorkerFlagDelegate);
	}
#endif
}

void ABenchmarkGymGameModeBase::TryAddSpatialMetrics()
{
	if (!GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
	{
		return;
	}

	USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GetNetDriver());
	if (ensure(SpatialDriver != nullptr))
	{
		USpatialMetrics* SpatialMetrics = SpatialDriver->SpatialMetrics;
		if (ensure(SpatialMetrics != nullptr))
		{
			AddSpatialMetrics(SpatialMetrics);
		}
	}
}

void ABenchmarkGymGameModeBase::AddSpatialMetrics(USpatialMetrics* SpatialMetrics)
{
	{
		// Valid on all workers
		UserSuppliedMetric Delegate;
		Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetFPSValid);
		SpatialMetrics->SetCustomMetric(AverageFPSValid, Delegate);
	}
	{
		UserSuppliedMetric Delegate;
		Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetActorCountValid);
		SpatialMetrics->SetCustomMetric(ActorCountValidMetricName, Delegate);
	}

	if (HasAuthority())
	{
		{
			UserSuppliedMetric Delegate;
			Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetClientRTT);
			SpatialMetrics->SetCustomMetric(AverageClientRTTMetricName, Delegate);
		}
		{
			UserSuppliedMetric Delegate;
			Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetClientUpdateTimeDelta);
			SpatialMetrics->SetCustomMetric(AverageClientUpdateTimeDeltaMetricName, Delegate);
		}
		{
			UserSuppliedMetric Delegate;
			Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetRequiredPlayersValid);
			SpatialMetrics->SetCustomMetric(ExpectedPlayersValidMetricName, Delegate);
		}
		{
			UserSuppliedMetric Delegate;
			Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetClientFPSValid);
			SpatialMetrics->SetCustomMetric(AverageClientFPSValid, Delegate);
		}
		{
			UserSuppliedMetric Delegate;
			Delegate.BindUObject(this, &ABenchmarkGymGameModeBase::GetPlayerMovement);
			SpatialMetrics->SetCustomMetric(PlayerMovementMetricName, Delegate);
		}
	}
}

void ABenchmarkGymGameModeBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TickServerFPSCheck(DeltaSeconds);
	TickClientFPSCheck(DeltaSeconds);
	TickPlayersConnectedCheck(DeltaSeconds);
	TickPlayersMovementCheck(DeltaSeconds);
	TickUXMetricCheck(DeltaSeconds);
	
	// PrintMetricsTimer needs to be reset at the the end of ABenchmarkGymGameModeBase::Tick.
	// This is so that the above function have a chance to run logic dependant on PrintMetricsTimer.HasTimerGoneOff().
	if (PrintMetricsTimer.HasTimerGoneOff())
	{
		PrintMetricsTimer.SetTimer(10);
	}
#if	STATS
	if (CPUProfileInterval > 0)
	{
		if (StatStartFileTimer.HasTimerGoneOff())
		{
			FString Cmd(TEXT("stat startfile"));
			if (GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
			{
				USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GetNetDriver());
				if (ensure(SpatialDriver != nullptr))
				{
					FString InFileName = FString::Printf(TEXT("%s-%s"), *SpatialDriver->Connection->GetWorkerId(), *FDateTime::Now().ToString(TEXT("%m.%d-%H.%M.%S")));
					const FString Filename = CreateProfileFilename(InFileName, TEXT(".ue4stats"), true);
					Cmd.Append(FString::Printf(TEXT(" %s"), *Filename));
				}
			}
			GEngine->Exec(GetWorld(), *Cmd);
			StatStartFileTimer.SetTimer(CPUProfileInterval);
		}
		if (StatStopFileTimer.HasTimerGoneOff())
		{
			GEngine->Exec(GetWorld(), TEXT("stat stopfile"));
			StatStopFileTimer.SetTimer(CPUProfileInterval);
		}
	}
#endif
#if !UE_BUILD_SHIPPING
	if (MemReportInterval > 0 && MemReportIntervalTimer.HasTimerGoneOff())
	{
		FString Cmd = TEXT("memreport -full");
		if (GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
		{
			USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GetNetDriver());
			if (ensure(SpatialDriver != nullptr))
			{
				Cmd.Append(FString::Printf(TEXT(" NAME=%s-%s"), *SpatialDriver->Connection->GetWorkerId(), *FDateTime::Now().ToString(TEXT("%m.%d-%H.%M.%S"))));
			}
		}
		GEngine->Exec(nullptr, *Cmd);
		MemReportIntervalTimer.SetTimer(MemReportInterval);
	}
#endif
}

void ABenchmarkGymGameModeBase::TickPlayersConnectedCheck(float DeltaSeconds)
{
	if (!HasAuthority())
	{
		return;
	}

	// Only check players once
	if (bHasRequiredPlayersCheckFailed)
	{
		return;
	}

	if (RequiredPlayerCheckTimer.HasTimerGoneOff() && !DeploymentValidTimer.HasTimerGoneOff())
	{
		const int32* ActorCount = TotalActorCounts.Find(SimulatedPawnClass);

		if (ActorCount == nullptr)
		{
			bHasRequiredPlayersCheckFailed = true;
			NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: Could not get Simulated Player actor count."), *NFRFailureString);
		}
		else if (*ActorCount >= RequiredPlayers)
		{
			RequiredPlayerCheckTimer.SetTimer(10);
			// Useful for NFR log inspection
			NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("All clients successfully connected. Required %d, got %d"), RequiredPlayers, *ActorCount);
		}
		else
		{
			bHasRequiredPlayersCheckFailed = true;
			// This log is used by the NFR pipeline to indicate if a client failed to connect
			NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: Client connection dropped. Required %d, got %d"), *NFRFailureString, RequiredPlayers, *ActorCount);
		}
		GetMetrics(MetricLeftLabel, ExpectedPlayersValidMetricName, MetricName, &ABenchmarkGymGameModeBase::GetRequiredPlayersValid);
	}
}

void ABenchmarkGymGameModeBase::TickPlayersMovementCheck(float DeltaSeconds)
{
	// Get velocity and report 
	GetVelocityForMovementReport();
	
	// Check velocity
	CheckVelocityForPlayerMovement();
}

void ABenchmarkGymGameModeBase::TickServerFPSCheck(float DeltaSeconds)
{
	// We have already failed so no need to keep checking
	if (bHasFpsFailed)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const UGDKTestGymsGameInstance* GameInstance = GetGameInstance<UGDKTestGymsGameInstance>();
	if (GameInstance == nullptr)
	{
		return;
	}

	const UNFRConstants* Constants = UNFRConstants::Get(World);
	check(Constants);

	const float FPS = GameInstance->GetAveragedFPS();

	if (FPS < Constants->GetMinServerFPS() &&
		Constants->ServerFPSMetricDelay.HasTimerGoneOff())
	{
		bHasFpsFailed = true;
		NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("%s: Server FPS check. FPS: %.8f"), *NFRFailureString, FPS);
	}

	GetMetrics(MetricLeftLabel, AverageFPSValid, MetricName, &ABenchmarkGymGameModeBase::GetFPSValid);
}

void ABenchmarkGymGameModeBase::TickClientFPSCheck(float DeltaSeconds)
{
	if (!HasAuthority())
	{
		return;
	}

	// We have already failed so no need to keep checking
	if (bHasClientFpsFailed)
	{
		return;
	}

	bool bClientFpsWasValid = true;
	for (TObjectIterator<UUserExperienceReporter> Itr; Itr; ++Itr) // These exist on player characters
	{
		UUserExperienceReporter* Component = *Itr;
		if (Component->GetOwner() != nullptr && Component->GetWorld() == GetWorld())
		{
			bClientFpsWasValid = bClientFpsWasValid && Component->bFrameRateValid; // Frame rate wait period is performed by the client and returned valid until then
		}
	}

	const UNFRConstants* Constants = UNFRConstants::Get(GetWorld());
	check(Constants);

	if (!bClientFpsWasValid &&
		Constants->ClientFPSMetricDelay.HasTimerGoneOff())
	{
		bHasClientFpsFailed = true;
		NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("%s: Client FPS check."), *NFRFailureString);
	}
	GetMetrics(MetricLeftLabel, AverageClientFPSValid, MetricName, &ABenchmarkGymGameModeBase::GetClientFPSValid);
}

void ABenchmarkGymGameModeBase::TickUXMetricCheck(float DeltaSeconds)
{
	UXAuthActorCount = 0;
	int ValidRTTCount = 0;
	int ValidUpdateTimeDeltaCount = 0;
	float ClientRTTMS = 0.0f;
	float ClientUpdateTimeDeltaMS = 0.0f;
	for (TObjectIterator<UUserExperienceReporter> Itr; Itr; ++Itr) // These exist on player characters
	{
		UUserExperienceReporter* Component = *Itr;
		if (Component->GetOwner() != nullptr && Component->HasBegunPlay() && Component->GetWorld() == GetWorld())
		{
			if (Component->ServerRTTMS > 0.f)
			{
				ClientRTTMS += Component->ServerRTTMS;
				ValidRTTCount++;
			}

			if (Component->ServerUpdateTimeDeltaMS > 0.f)
			{
				ClientUpdateTimeDeltaMS += Component->ServerUpdateTimeDeltaMS;
				ValidUpdateTimeDeltaCount++;
			}

			if (Component->GetOwner()->HasAuthority())
			{
				UXAuthActorCount++;
			}
		}
	}	

	ClientRTTMS /= static_cast<float>(ValidRTTCount) + 0.00001f; // Avoid div 0
	ClientUpdateTimeDeltaMS /= static_cast<float>(ValidUpdateTimeDeltaCount) + 0.00001f; // Avoid div 0

	if (PrintMetricsTimer.HasTimerGoneOff())
	{
		NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("UX metric values. RTT: %.8f(%d), UpdateDelta: %.8f(%d)"), ClientRTTMS, ValidRTTCount, ClientUpdateTimeDeltaMS, ValidUpdateTimeDeltaCount);
	}

	if (PrintMetricsTimer.HasTimerGoneOff() || HasAuthority())
	{
		ReportUserExperience(GetGameInstance()->GetSpatialWorkerId(), ClientRTTMS, ClientUpdateTimeDeltaMS);
	}

}

void ABenchmarkGymGameModeBase::ParsePassedValues()
{
	const FString& CommandLine = FCommandLine::Get();

	// Always read profiling feature details from cmd line as it's not setup for worker flags
#if	STATS
	FString CPUProfileString;
	if (FParse::Value(*CommandLine, *StatProfileCommandLineKey, CPUProfileString))
	{
		InitStatTimer(CPUProfileString);
	}
	else 
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("The CPU profile duration and interval are not set. "));
	}


#endif
#if !UE_BUILD_SHIPPING
	FString MemReportIntervalString;
	if (FParse::Value(*CommandLine, *MemRemportIntervalKey, MemReportIntervalString))
	{
		InitMemReportTimer(MemReportIntervalString);
	}
	else 
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("The memreport interval is not set. "));
	}

#endif

	if (FParse::Param(*CommandLine, *ReadFromCommandLineKey))
	{
		ReadCommandLineArgs(CommandLine);
	}
	else if (GetDefault<UGeneralProjectSettings>()->UsesSpatialNetworking())
	{
		USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GetNetDriver());
		if (ensure(SpatialDriver != nullptr))
		{
			USpatialWorkerFlags* SpatialWorkerFlags = SpatialDriver->SpatialWorkerFlags;
			if (ensure(SpatialWorkerFlags != nullptr))
			{
				ReadWorkerFlagValues(SpatialWorkerFlags);
			}
		}
	}
}

void ABenchmarkGymGameModeBase::ReadCommandLineArgs(const FString& CommandLine)
{
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Found ReadFromCommandLine in command line Keys, worker flags for custom spawning will be ignored."));

	FParse::Value(*CommandLine, *TotalPlayerCommandLineKey, ExpectedPlayers);
	FParse::Value(*CommandLine, *RequiredPlayersCommandLineKey, RequiredPlayers);

	int32 NumNPCs = 0;
	FParse::Value(*CommandLine, *TotalNPCsCommandLineKey, NumNPCs);
	SetTotalNPCs(NumNPCs);

	int32 Lifetime = 0;
	FParse::Value(*CommandLine, *TestLiftimeCommandLineKey, Lifetime);
	SetLifetime(Lifetime);

	FParse::Value(*CommandLine, *MaxRoundTripCommandLineKey, MaxClientRoundTripMS);
	FParse::Value(*CommandLine, *MaxUpdateTimeDeltaCommandLineKey, MaxClientUpdateTimeDeltaMS);

	FParse::Value(*CommandLine, *CubeRespawnBaseTimeCommandLineKey, CubeRespawnBaseTime);
	FParse::Value(*CommandLine, *CubeRespawnRandomRangeCommandLineKey, CubeRespawnRandomRangeTime);

	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Players %d, RequiredPlayers %d, NPCs %d, RoundTrip %d, UpdateTimeDelta %d, CubeRespawnBaseTime %f, CubeRespawnRandomRangeTime %f"),
		ExpectedPlayers, RequiredPlayers, TotalNPCs, MaxClientRoundTripMS, MaxClientUpdateTimeDeltaMS, CubeRespawnBaseTime, CubeRespawnRandomRangeTime);
}

void ABenchmarkGymGameModeBase::ReadWorkerFlagValues(USpatialWorkerFlags* SpatialWorkerFlags)
{
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Using worker flags to load custom spawning parameters."));
	FString ExpectedPlayersString, RequiredPlayersString, TotalNPCsString, MaxRoundTrip, MaxUpdateTimeDelta, LifetimeString, NumWorkersString, CubeRespawnBaseTimeString, CubeRespawnRandomRangeTimeString;

	if (SpatialWorkerFlags->GetWorkerFlag(TotalPlayerWorkerFlag, ExpectedPlayersString))
	{
		ExpectedPlayers = FCString::Atoi(*ExpectedPlayersString);
	}

	if (SpatialWorkerFlags->GetWorkerFlag(RequiredPlayersWorkerFlag, RequiredPlayersString))
	{
		RequiredPlayers = FCString::Atoi(*RequiredPlayersString);
	}

	if (SpatialWorkerFlags->GetWorkerFlag(TotalNPCsWorkerFlag, TotalNPCsString))
	{
		SetTotalNPCs(FCString::Atoi(*TotalNPCsString));
	}

	if (SpatialWorkerFlags->GetWorkerFlag(MaxRoundTripWorkerFlag, MaxRoundTrip))
	{
		MaxClientRoundTripMS = FCString::Atoi(*MaxRoundTrip);
	}

	if (SpatialWorkerFlags->GetWorkerFlag(MaxUpdateTimeDeltaWorkerFlag, MaxUpdateTimeDelta))
	{
		MaxClientUpdateTimeDeltaMS = FCString::Atoi(*MaxUpdateTimeDelta);
	}

	if (SpatialWorkerFlags->GetWorkerFlag(TestLiftimeWorkerFlag, LifetimeString))
	{
		SetLifetime(FCString::Atoi(*LifetimeString));
	}

	if (SpatialWorkerFlags->GetWorkerFlag(CubeRespawnBaseTimeWorkerFlag, CubeRespawnBaseTimeString))
	{
		CubeRespawnBaseTime = FCString::Atof(*CubeRespawnBaseTimeString);
	}

	if (SpatialWorkerFlags->GetWorkerFlag(CubeRespawnRandomRangeTimeWorkerFlag, CubeRespawnRandomRangeTimeString))
	{
		CubeRespawnRandomRangeTime = FCString::Atof(*CubeRespawnRandomRangeTimeString);
	}

#if	STATS
	FString CPUProfileString;
	if (SpatialWorkerFlags->GetWorkerFlag(StatProfileWorkerFlag, CPUProfileString))
	{
		InitStatTimer(CPUProfileString);
	}

	FString MemReportIntervalString;
	if (SpatialWorkerFlags->GetWorkerFlag(MemReportFlag, MemReportIntervalString))
	{
		InitMemReportTimer(MemReportIntervalString);
	}
#endif

	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Players %d, RequiredPlayers %d, NPCs %d, RoundTrip %d, UpdateTimeDelta %d, CubeRespawnBaseTime %f, CubeRespawnRandomRangeTime %f"),
		ExpectedPlayers, RequiredPlayers, TotalNPCs, MaxClientRoundTripMS, MaxClientUpdateTimeDeltaMS, CubeRespawnBaseTime, CubeRespawnRandomRangeTime);
}

void ABenchmarkGymGameModeBase::SetTotalNPCs(int32 Value)
{
	if (Value != TotalNPCs)
	{
		TotalNPCs = Value;
		OnTotalNPCsUpdated(TotalNPCs);
	}
}

void ABenchmarkGymGameModeBase::OnRepTotalNPCs()
{
	OnTotalNPCsUpdated(TotalNPCs);
}

void ABenchmarkGymGameModeBase::OnActorCountReportIdx()
{
	UpdateAndReportActorCounts();
}

void ABenchmarkGymGameModeBase::UpdateAndReportActorCounts()
{
	const UWorld* World = GetWorld();
	bool bSpatialEnabled = USpatialStatics::IsSpatialNetworkingEnabled();
	const FString WorkerID = bSpatialEnabled ? GetGameInstance()->GetSpatialWorkerId() : TEXT("Worker1");
	if (WorkerID.IsEmpty())
	{
		NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: Worker ID was empty"), *NFRFailureString);
		return;
	}

	ActorCountMap& ThisWorkerActorCounts = WorkerActorCounts.FindOrAdd(WorkerID);
	for (auto const& Pair : ExpectedActorCounts)
	{
		const TSubclassOf<AActor>& ActorClass = Pair.Key;
		const FExpectedActorCountConfig& Config = Pair.Value;
		if (Config.MinCount > 0)
		{
			int32 TotalCount = 0;
			int32& AuthCount = ThisWorkerActorCounts.FindOrAdd(ActorClass);
			GetActorCount(ActorClass, TotalCount, AuthCount);
			NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Local Actor Count - ActorClass: %s Count: %d, AuthCount: %d"), *ActorClass->GetName(), TotalCount, AuthCount);
		}
	}

	TArray<FActorCount> ActorCountArray;
	ActorCountArray.Reserve(ThisWorkerActorCounts.Num());

	for (const auto& Pair : ThisWorkerActorCounts)
	{
		ActorCountArray.Add(FActorCount(Pair.Key, Pair.Value));
	}

	ReportAuthoritativeActorCount(ActorCountReportIdx, WorkerID, ActorCountArray);
}

void ABenchmarkGymGameModeBase::GetActorCount(const TSubclassOf<AActor>& ActorClass, int32& OutTotalCount, int32& OutAuthCount) const
{
	const UWorld* World = GetWorld();
	USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(World->GetNetDriver());

	TArray<AActor*> Actors;
	UGameplayStatics::GetAllActorsOfClass(World, ActorClass, Actors);

	OutAuthCount = 0;
	for (const AActor* Actor : Actors)
	{
		if (Actor->HasAuthority())
		{
			OutAuthCount++;
		}
		else if (SpatialDriver != nullptr)
		{
			// During actor authority handover, there's a period where no server will believe it has authority over
			// the Unreal actor, but will still have authority over the entity. To better minimize this period, use
			// the spatial authority as a fallback validation.
			Worker_EntityId EntityId = SpatialDriver->PackageMap->GetEntityIdFromObject(Actor);
			const SpatialGDK::EntityViewElement* Element = SpatialDriver->Connection->GetView().Find(EntityId);
			if (Element != nullptr && Element->Authority.Contains(SpatialConstants::SERVER_AUTH_COMPONENT_SET_ID))
			{
				OutAuthCount++;
			}
		}
	}

	OutTotalCount = Actors.Num();
}

void ABenchmarkGymGameModeBase::SetLifetime(int32 Lifetime)
{
	if (TestLifetimeTimer.SetTimer(Lifetime))
	{
		TestLifetimeTimer.SetLock(true);
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Setting NFR test lifetime %d"), Lifetime);
	}
	else
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Warning, TEXT("Could not set NFR test liftime to %d. Timer was locked."), Lifetime);
	}
}

void ABenchmarkGymGameModeBase::ReportAuthoritativePlayerMovement_Implementation(const FString& WorkerID, const FVector2D& AverageData)
{
	if (!HasAuthority())
	{
		return;
	}

	LatestAvgVelocityMap.Emplace(WorkerID, AverageData);

	float TotalPlayers = 0.000001f;	// Avoid divide zero.
	float TotalVelocity = 0.0f;
	for (const auto& KeyValue : LatestAvgVelocityMap)
	{
		TotalVelocity += KeyValue.Value.X;
		TotalPlayers += KeyValue.Value.Y;
	}

	CurrentPlayerAvgVelocity = TotalVelocity / TotalPlayers;
}

void ABenchmarkGymGameModeBase::ReportUserExperience_Implementation(const FString& WorkerID, float RTTime, float UpdateTime)
{
	check(HasAuthority());

	if (!WorkerID.IsEmpty())
	{
		LatestClientUXMap.Emplace(WorkerID, UX{RTTime, UpdateTime});
	}

	AveragedClientRTTMS = 0.f;
	AveragedClientUpdateTimeDeltaMS = 0.f;

	// To extend this functionality to multi-worker, we store each server's averaged client UX metrics individually, and then iterate through
	// each averaged value identifying the worst/max UX metric. We then check that worse averaged value against the failure threshold.
	for (const auto& Entry : LatestClientUXMap)
	{
		AveragedClientRTTMS = FMath::Max(AveragedClientRTTMS, Entry.Value.RTT);
		AveragedClientUpdateTimeDeltaMS = FMath::Max(AveragedClientUpdateTimeDeltaMS, Entry.Value.UpdateTime);
	}

	GetMetrics(MetricLeftLabel, AverageClientRTTMetricName, MetricName, &ABenchmarkGymGameModeBase::GetClientRTT);
	GetMetrics(MetricLeftLabel, AverageClientUpdateTimeDeltaMetricName, MetricName, &ABenchmarkGymGameModeBase::GetClientUpdateTimeDelta);

	const bool bUXMetricValid = AveragedClientRTTMS <= MaxClientRoundTripMS && AveragedClientUpdateTimeDeltaMS <= MaxClientUpdateTimeDeltaMS;

	const UNFRConstants* Constants = UNFRConstants::Get(GetWorld());
	check(Constants);
	if (!bHasUxFailed &&
		!bUXMetricValid &&
		Constants->UXMetricDelay.HasTimerGoneOff())
	{
		bHasUxFailed = true;
		NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: UX metric check. RTT: %.8f, UpdateDelta: %.8f"), *NFRFailureString, AveragedClientRTTMS, AveragedClientUpdateTimeDeltaMS);
	}
}

#if	STATS
void ABenchmarkGymGameModeBase::InitStatTimer(const FString& CPUProfileString)
{
	FString CPUProfileIntervalString, CPUProfileDurationString;
	if (CPUProfileString.Split(TEXT("&"), &CPUProfileIntervalString, &CPUProfileDurationString))
	{
		int32 FirstStartCPUProfile = FCString::Atoi(*CPUProfileIntervalString);
		int32 CPUProfileDuration = FCString::Atoi(*CPUProfileDurationString);
		StatStartFileTimer.SetTimer(FirstStartCPUProfile);
		StatStopFileTimer.SetTimer(FirstStartCPUProfile + CPUProfileDuration);
		CPUProfileInterval = FirstStartCPUProfile + CPUProfileDuration;
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("CPU profile interval is set to %ds, duration is set to %ds"), FirstStartCPUProfile, CPUProfileDuration);
	}
	else
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Please ensure both CPU profile interval and duration are set properly"));
	}
}
#endif
#if !UE_BUILD_SHIPPING
void ABenchmarkGymGameModeBase::InitMemReportTimer(const FString& MemReportIntervalString)
{
	MemReportInterval = FCString::Atoi(*MemReportIntervalString);
	MemReportIntervalTimer.SetTimer(MemReportInterval);
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("MemReport Interval is set to %d seconds"), MemReportInterval);
}
#endif

void ABenchmarkGymGameModeBase::ReportAuthoritativeActorCount_Implementation(const int32 WorkerActorCountReportIdx, const FString& WorkerID, const TArray<FActorCount>& ActorCounts)
{
	ActorCountMap& ActorCountMap = WorkerActorCounts.FindOrAdd(WorkerID);
	for (const FActorCount& ActorCount : ActorCounts)
	{
		ActorCountMap.FindOrAdd(ActorCount.ActorClass) = ActorCount.Count;
	}

	ActorCountReportedIdxs.FindOrAdd(WorkerID) = WorkerActorCountReportIdx;
	if (ActorCountReportedIdxs.Num() == NumWorkers)
	{
		bool bAllWorkersInSync = true;
		for (const auto& Pair : ActorCountReportedIdxs)
		{
			if (Pair.Value != WorkerActorCountReportIdx)
			{
				bAllWorkersInSync = false;
				break;
			}
		}

		if (bAllWorkersInSync)
		{
			UpdateAndCheckTotalActorCounts();
		}
	}
}

void ABenchmarkGymGameModeBase::UpdateAndCheckTotalActorCounts()
{
	// Clear the failure timer as we are able to calculate actor count totals.
	FTimerManager& TimerManager = GetWorld()->GetTimerManager();
	TimerManager.ClearTimer(FailActorCountTimeoutTimerHandle);

	const UNFRConstants* Constants = UNFRConstants::Get(GetWorld());
	check(Constants);

	if (!Constants->ActorCheckDelay.HasTimerGoneOff())
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Not ready to consider actor count metric"));
	}

	if (TestLifetimeTimer.HasTimerGoneOff())
	{
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Test lifetime over. Will not consider actor count metric"));
	}

	ActorCountMap TempTotalActorCounts;
	for (const auto& WorkerPair : WorkerActorCounts)
	{
		const FString& WorkerId = WorkerPair.Key;
		const ActorCountMap& SpecificWorkerActorCounts = WorkerPair.Value;

		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("--- Actor Count for Worker: %s ---"), *WorkerId);

		for (const auto& ActorCountPair : SpecificWorkerActorCounts)
		{
			const TSubclassOf<AActor>& ActorClass = ActorCountPair.Key;
			const int32& ActorCount = ActorCountPair.Value;

			int32& TotalActorCount = TempTotalActorCounts.FindOrAdd(ActorClass);
			TotalActorCount += ActorCount;

			UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Class: %s, Total: %d"), *ActorClass->GetName(), ActorCount);
		}
	}

	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("--- Actor Count Totals ---"));

	bActorCountFailureState = false; // Reset check.
	for (const auto& ActorCountPair : TempTotalActorCounts)
	{
		const TSubclassOf<AActor>& ActorClass = ActorCountPair.Key;
		const int32 TotalActorCount = ActorCountPair.Value;

		TotalActorCounts.FindOrAdd(ActorClass) = TotalActorCount;
		UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Class: %s, Total: %d"), *ActorClass->GetName(), TotalActorCount);

		const bool bIsReadyToConsiderActorCount = Constants->ActorCheckDelay.HasTimerGoneOff() && !TestLifetimeTimer.HasTimerGoneOff();
		if (bIsReadyToConsiderActorCount)
		{
			// Check for test failure
			const FExpectedActorCountConfig& ExpectedActorCount = ExpectedActorCounts[ActorClass];
			if (TotalActorCount < ExpectedActorCount.MinCount || TotalActorCount > ExpectedActorCount.MaxCount)
			{
				bActorCountFailureState = true;
				if (!bHasActorCountFailed)
				{
					bHasActorCountFailed = true;
					NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s: Unreal actor count check. ObjectClass %s, MinCount %d, MaxCount %d, ActualCount %d"),
						*NFRFailureString,
						*ActorClass->GetName(),
						ExpectedActorCount.MinCount,
						ExpectedActorCount.MaxCount,
						TotalActorCount);
				}
			}
			GetMetrics(MetricLeftLabel, ActorCountValidMetricName, MetricName, &ABenchmarkGymGameModeBase::GetActorCountValid);
		}
	}
}

void ABenchmarkGymGameModeBase::GetVelocityForMovementReport()
{
	// Report logic
	if (RequiredPlayerMovementReportTimer.HasTimerGoneOff())
	{
		FVector2D AvgVelocity = FVector2D(0.0f, 0.000001f);
		// Loop each players
		GetPlayersVelocitySum(AvgVelocity);

		// Avg
		AvgVelocity.X /= AvgVelocity.Y;

		// Report
		ReportAuthoritativePlayerMovement(GetGameInstance()->GetSpatialWorkerId(), AvgVelocity);

		RequiredPlayerMovementReportTimer.SetTimer(29);
	}
}

void ABenchmarkGymGameModeBase::GetPlayersVelocitySum(FVector2D& Velocity)
{
	for (FConstPlayerControllerIterator PCIt = GetWorld()->GetPlayerControllerIterator(); PCIt; ++PCIt)
	{
		APlayerController* PC = PCIt->Get();
		if (!PC || !PC->HasAuthority())
			continue;
		auto PlayerPawn = PC->GetPawn();
		if (!PlayerPawn)
			continue;
		UCharacterMovementComponent* Component = Cast<UCharacterMovementComponent>(PlayerPawn->GetMovementComponent());
		if (Component != nullptr)
		{
			Velocity.X += Component->Velocity.Size();
			Velocity.Y += 1;
		}
	}
}

void ABenchmarkGymGameModeBase::CheckVelocityForPlayerMovement()
{
	if (!HasAuthority() || !RequiredPlayerMovementCheckTimer.HasTimerGoneOff())
		return;

	AvgVelocityHistory.Add(CurrentPlayerAvgVelocity);
	if (AvgVelocityHistory.Num() > 30)
	{
		AvgVelocityHistory.RemoveAt(0);
	}
	RecentPlayerAvgVelocity = 0.0f;
	for (auto Velocity : AvgVelocityHistory)
	{
		RecentPlayerAvgVelocity += Velocity;
	}
	RecentPlayerAvgVelocity /= (AvgVelocityHistory.Num() + 0.01f);
	GetMetrics(MetricLeftLabel, PlayerMovementMetricName, MetricName, &ABenchmarkGymGameModeBase::GetPlayerMovement);

	RequiredPlayerMovementCheckTimer.SetTimer(30);
	
	// Extra step for native scenario.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		const UNFRConstants* Constants = UNFRConstants::Get(World);
		check(Constants);

		if (RecentPlayerAvgVelocity > Constants->GetMinPlayerAvgVelocity())
		{
			NFR_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Check players' average velocity. Current velocity=%.1f"), RecentPlayerAvgVelocity);
		}
		else
		{
			NFR_LOG(LogBenchmarkGymGameModeBase, Error, TEXT("%s:Players' average velocity is too small. Current velocity=%.1f"), *NFRFailureString, RecentPlayerAvgVelocity);
		}
	}
}

void ABenchmarkGymGameModeBase::OutputPlayerDensity()
{
	// Outputs the count that each NPC and Simulated Player falls into each of the QBI-F bucket types. This is not performant but
	// is only used for debugging purposes currently and isn't enabled by default.
	FTimerHandle CountTimer;
	GetWorld()->GetTimerManager().SetTimer(
		CountTimer,
		[WeakThis = TWeakObjectPtr<ABenchmarkGymGameModeBase>(this)]() {
		if (ABenchmarkGymGameModeBase* GameMode = WeakThis.Get())
		{
			USpatialNetDriver* SpatialDriver = Cast<USpatialNetDriver>(GameMode->GetNetDriver());

			TArray<AActor*> PlayerControllers, PlayerCharacters, NPCs, AllCharacters;
			UGameplayStatics::GetAllActorsOfClass(GameMode->GetWorld(), GameMode->SimulatedPlayerControllerClass, PlayerControllers);
			UGameplayStatics::GetAllActorsOfClass(GameMode->GetWorld(), GameMode->SimulatedPawnClass, PlayerCharacters);
			UGameplayStatics::GetAllActorsOfClass(GameMode->GetWorld(), GameMode->NPCClass, NPCs);
			AllCharacters = PlayerCharacters;
			AllCharacters.Append(NPCs);

			const USpatialGDKSettings* SpatialGDKSettings = GetDefault<USpatialGDKSettings>();
			TArray<float> NCDDistanceRatios;
			NCDDistanceRatios.Push(SpatialGDKSettings->FullFrequencyNetCullDistanceRatio);
			
			for (const auto& Pair : SpatialGDKSettings->InterestRangeFrequencyPairs)
			{
				NCDDistanceRatios.Push(Pair.DistanceRatio);
			}
			NCDDistanceRatios.Sort();

			FString DistanceRatiosAsString(TEXT("Distance ratios to NCD: "));
			for (float Ratio : NCDDistanceRatios)
			{
				DistanceRatiosAsString += FString::Format(TEXT(" {0}"), { Ratio });
			}
			UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("%s"), *DistanceRatiosAsString);

			TArray<int> TotalCountPerBucket;
			TArray<int> CountPerBucket;
			const int NumBuckets = NCDDistanceRatios.Num() + 1; // Add extra bucket for actors outside interest
			TotalCountPerBucket.Init(0, NumBuckets); 

			for (const AActor* PlayerController : PlayerControllers)
			{
				CountPerBucket.Init(0, NumBuckets);

				FVector Pos = SpatialDriver->GetActorChannelByEntityId(SpatialDriver->GetActorEntityId(*PlayerController))->GetLastUpdatedSpatialPosition();
				for (const AActor* Character : AllCharacters)
				{
					FVector OtherPos = SpatialDriver->GetActorChannelByEntityId(SpatialDriver->GetActorEntityId(*Character))->GetLastUpdatedSpatialPosition();
					float Dist = FVector::Distance(Pos, OtherPos);
					float NCD = FMath::Sqrt(Character->NetCullDistanceSquared);
					int Idx = NCDDistanceRatios.Num();
					for (int i = 0; i < NCDDistanceRatios.Num(); ++i)
					{
						if (Dist < NCDDistanceRatios[i] * NCD)
						{
							Idx = i;
							break;
						}
					}
					CountPerBucket[Idx]++;
					TotalCountPerBucket[Idx]++;
				}

				int TotalCount = 0;
				FString CountsAsString;
				for (int Count : CountPerBucket)
				{
					CountsAsString += FString::Format(TEXT(" {0}"), { Count });
					TotalCount += Count;
				}

				UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Density: %s (%d)"), *CountsAsString, TotalCount);
			}

			int TotalCount = 0;
			FString CountsAsString;
			for (int Count : TotalCountPerBucket)
			{
				CountsAsString += FString::Format(TEXT(" {0}"), { Count });
				TotalCount += Count;
			}
			UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("Density for all: %s (%d)"), *CountsAsString, TotalCount);
		}
	}, 5.f, true);
}

void ABenchmarkGymGameModeBase::OnExpectedPlayersFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	ExpectedPlayers = FCString::Atoi(*FlagValue);
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("ExpectedPlayers %d"), ExpectedPlayers);
}

void ABenchmarkGymGameModeBase::OnRequiredPlayersFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	RequiredPlayers = FCString::Atoi(*FlagValue);
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("RequiredPlayers %d"), RequiredPlayers);
}

void ABenchmarkGymGameModeBase::OnTotalNPCsFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	SetTotalNPCs(FCString::Atoi(*FlagValue));
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("TotalNPCs %d"), TotalNPCs);
}

void ABenchmarkGymGameModeBase::OnMaxRoundTripFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	MaxClientRoundTripMS = FCString::Atoi(*FlagValue);
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("MaxClientRoundTripMS %d"), MaxClientRoundTripMS);
}

void ABenchmarkGymGameModeBase::OnMaxUpdateTimeDeltaFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	MaxClientUpdateTimeDeltaMS = FCString::Atoi(*FlagValue);
	UE_LOG(LogBenchmarkGymGameModeBase, Log, TEXT("MaxClientUpdateTimeDeltaMS %d"), MaxClientUpdateTimeDeltaMS);
}

void ABenchmarkGymGameModeBase::OnTestLiftimeFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	SetLifetime(FCString::Atoi(*FlagValue));
}

void ABenchmarkGymGameModeBase::OnCubeRespawnBaseTimeFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
	CubeRespawnBaseTime = FCString::Atof(*FlagValue);
}

void ABenchmarkGymGameModeBase::OnCubeRespawnRandomRangeTimeUpdate(const FString& FlagName, const FString& FlagValue)
{
	CubeRespawnRandomRangeTime = FCString::Atof(*FlagValue);
}

void ABenchmarkGymGameModeBase::OnStatProfileFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
#if	STATS
	InitStatTimer(FlagValue);
#endif
}

void ABenchmarkGymGameModeBase::OnMemReportFlagUpdate(const FString& FlagName, const FString& FlagValue)
{
#if	STATS
	InitMemReportTimer(FlagValue);
#endif
}

void ABenchmarkGymGameModeBase::GetMetrics(const FString& LeftLabel, const FString& RightLabel, const FString& MetricsName, ABenchmarkGymGameModeBase::FunctionPtrType Func)
{
	TSharedPtr<FPrometheusMetric> MetricsPtr = UMetricsBlueprintLibrary::GetMetric(MetricsName, TArray<FPrometheusLabel>{ TPair<FString, FString>(LeftLabel, RightLabel), TPair<FString,FString>(MetricEnginePlatformLeftLabel,MetricEnginePlatformRightLabel) });
	if (MetricsPtr.IsValid())
	{
		auto Value = (this->*(Func))();
		MetricsPtr->Set(Value);
	}
}
