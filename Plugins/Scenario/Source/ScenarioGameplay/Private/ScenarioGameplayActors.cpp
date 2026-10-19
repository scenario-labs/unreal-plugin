// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioGameplayActors.h"

#include "Components/AudioComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "SScenarioGameHud.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "ScenarioGameplay"

namespace
{
	bool IsPlayer(const AActor* Actor)
	{
		const APawn* Pawn = Cast<APawn>(Actor);
		return Pawn && Pawn->IsPlayerControlled();
	}

	UPointLightComponent* MakeGlow(AActor* Owner, USceneComponent* Parent, float Candelas, float Radius)
	{
		UPointLightComponent* Light = Owner->CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
		Light->SetupAttachment(Parent);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(Candelas);
		Light->SetAttenuationRadius(Radius);
		Light->SetLightColor(FLinearColor(1.0f, 0.8f, 0.45f));
		Light->SetCastShadows(false);
		return Light;
	}

	FText Clock(float Seconds)
	{
		const int32 Whole = FMath::Max(0, FMath::CeilToInt(Seconds));
		return FText::FromString(FString::Printf(TEXT("%02d:%02d"), Whole / 60, Whole % 60));
	}
}

// Pickup

AScenarioPickup::AScenarioPickup()
{
	PrimaryActorTick.bCanEverTick = true;

	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	Trigger->InitSphereRadius(110.0f);
	Trigger->SetCollisionProfileName(TEXT("Trigger"));
	Trigger->SetGenerateOverlapEvents(true);
	RootComponent = Trigger;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Trigger);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);

	Glow = MakeGlow(this, Trigger, 30.0f, 450.0f);
}

void AScenarioPickup::BeginPlay()
{
	Super::BeginPlay();
	MeshBase = Mesh->GetRelativeLocation();
	// Pickups next to each other bob out of step.
	Age = FMath::Frac(GetActorLocation().X * 0.0137f + GetActorLocation().Y * 0.0071f) * 6.0f;
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AScenarioPickup::HandleOverlap);
}

void AScenarioPickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	Mesh->SetRelativeLocation(MeshBase + FVector(0.0f, 0.0f, FMath::Sin(Age * 2.2f) * BobHeight));
	Mesh->AddRelativeRotation(FRotator(0.0f, SpinDegreesPerSecond * DeltaSeconds, 0.0f));
}

void AScenarioPickup::HandleOverlap(UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (bCollected || !IsPlayer(OtherActor))
	{
		return;
	}
	AScenarioGameRules* Rules = AScenarioGameRules::Find(this);
	if (Rules && Rules->GetGameState() != EScenarioGameState::Playing)
	{
		return;
	}
	bCollected = true;
	if (PickupSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, PickupSound, GetActorLocation());
	}
	OnCollected.Broadcast();
	if (Rules)
	{
		Rules->NotifyPickup(this);
	}
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	SetLifeSpan(0.5f);
}

// Goal

AScenarioGoal::AScenarioGoal()
{
	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	Trigger->InitBoxExtent(FVector(150.0f, 150.0f, 150.0f));
	Trigger->SetCollisionProfileName(TEXT("Trigger"));
	Trigger->SetGenerateOverlapEvents(true);
	RootComponent = Trigger;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Trigger);

	Glow = MakeGlow(this, Trigger, 80.0f, 900.0f);
	Glow->SetRelativeLocation(FVector(0.0f, 0.0f, 150.0f));
}

void AScenarioGoal::BeginPlay()
{
	Super::BeginPlay();
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &AScenarioGoal::HandleOverlap);
}

void AScenarioGoal::HandleOverlap(UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (!IsPlayer(OtherActor))
	{
		return;
	}
	if (AScenarioGameRules* Rules = AScenarioGameRules::Find(this))
	{
		Rules->NotifyGoal(this);
	}
}

// Rules

AScenarioGameRules::AScenarioGameRules()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite)
	{
		Sprite->SetupAttachment(RootComponent);
	}
	Title = LOCTEXT("DefaultTitle", "Scenario Game");
	PickupName = LOCTEXT("DefaultPickups", "pickups");
}

AScenarioGameRules* AScenarioGameRules::Find(const UObject* WorldContext)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return nullptr;
	}
	TActorIterator<AScenarioGameRules> It(World);
	return It ? *It : nullptr;
}

bool AScenarioGameRules::HasGoal() const
{
	return static_cast<bool>(TActorIterator<AScenarioGoal>(GetWorld()));
}

void AScenarioGameRules::BeginPlay()
{
	Super::BeginPlay();
	Total = 0;
	for (TActorIterator<AScenarioPickup> It(GetWorld()); It; ++It)
	{
		++Total;
	}
	TimeLeft = TimeLimitSeconds;
	if (Music)
	{
		MusicComponent = UGameplayStatics::SpawnSound2D(this, Music, MusicVolume, 1.0f, 0.0f, nullptr, false, false);
		if (MusicComponent)
		{
			MusicComponent->OnAudioFinished.AddDynamic(this, &AScenarioGameRules::LoopMusic);
		}
	}
	if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
	{
		Hud = SNew(SScenarioGameHud).Rules(this);
		Viewport->AddViewportWidgetContent(Hud.ToSharedRef(), 10);
	}
	Toast(GetObjectiveText(), 5.0f);
}

void AScenarioGameRules::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Hud.IsValid())
	{
		if (UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
		{
			Viewport->RemoveViewportWidgetContent(Hud.ToSharedRef());
		}
		Hud.Reset();
	}
	if (MusicComponent)
	{
		MusicComponent->OnAudioFinished.RemoveAll(this);
		MusicComponent->Stop();
	}
	Super::EndPlay(Reason);
}

void AScenarioGameRules::LoopMusic()
{
	if (MusicComponent && State != EScenarioGameState::Lost)
	{
		MusicComponent->Play();
	}
}

void AScenarioGameRules::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (State != EScenarioGameState::Playing)
	{
		return;
	}
	Elapsed += DeltaSeconds;
	if (TimeLimitSeconds > 0.0f)
	{
		TimeLeft -= DeltaSeconds;
		if (TimeLeft <= 0.0f)
		{
			TimeLeft = 0.0f;
			Lose();
		}
	}
}

void AScenarioGameRules::Toast(const FText& Text, float Seconds)
{
	ToastText = Text;
	ToastUntil = GetWorld()->GetTimeSeconds() + Seconds;
}

void AScenarioGameRules::NotifyPickup(AScenarioPickup*)
{
	if (State != EScenarioGameState::Playing)
	{
		return;
	}
	Collected = FMath::Min(Collected + 1, Total);
	if (Collected < Total)
	{
		Toast(FText::Format(LOCTEXT("PickupToast", "{0} of {1} {2}"), Collected, Total, PickupName));
	}
	else if (HasGoal())
	{
		Toast(FText::Format(LOCTEXT("AllFound", "All {0} found. Reach the goal!"), PickupName), 4.0f);
	}
	else
	{
		Win();
	}
}

void AScenarioGameRules::NotifyGoal(AScenarioGoal* Goal)
{
	if (State != EScenarioGameState::Playing)
	{
		return;
	}
	if (Goal && Goal->bNeedsAllPickups && Collected < Total)
	{
		Toast(FText::Format(LOCTEXT("NeedMore", "Find {0} more {1} first"), Total - Collected, PickupName));
		return;
	}
	Win();
}

void AScenarioGameRules::Win()
{
	State = EScenarioGameState::Won;
	EndText = LOCTEXT("Won", "You made it!");
	EndSubText = FText::Format(LOCTEXT("WonSub", "{0} {1} in {2}"), Collected, PickupName, Clock(Elapsed));
	ToastUntil = 0.0f;
	if (WinSound)
	{
		UGameplayStatics::PlaySound2D(this, WinSound);
	}
	OnWin.Broadcast();
}

void AScenarioGameRules::Lose()
{
	State = EScenarioGameState::Lost;
	EndText = LOCTEXT("Lost", "Time's up");
	EndSubText = RestartDelaySeconds > 0.0f
		? FText::Format(LOCTEXT("LostRestart", "{0} of {1} {2}. Starting again..."), Collected, Total, PickupName)
		: FText::Format(LOCTEXT("LostSub", "{0} of {1} {2}"), Collected, Total, PickupName);
	ToastUntil = 0.0f;
	if (MusicComponent)
	{
		MusicComponent->FadeOut(2.0f, 0.0f);
	}
	if (LoseSound)
	{
		UGameplayStatics::PlaySound2D(this, LoseSound);
	}
	OnLose.Broadcast();
	if (RestartDelaySeconds > 0.0f)
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
			{
				Controller->RestartLevel();
			}
		}), RestartDelaySeconds, false);
	}
}

FText AScenarioGameRules::GetObjectiveText() const
{
	if (!Objective.IsEmpty())
	{
		return Objective;
	}
	return HasGoal()
		? FText::Format(LOCTEXT("ObjectiveGoal", "Collect every {0}, then reach the goal."), PickupName)
		: FText::Format(LOCTEXT("ObjectiveAll", "Collect every {0}."), PickupName);
}

FText AScenarioGameRules::GetProgressText() const
{
	return FText::Format(LOCTEXT("Progress", "{0} / {1} {2}"), Collected, Total, PickupName);
}

FText AScenarioGameRules::GetTimerText() const
{
	return TimeLimitSeconds > 0.0f ? Clock(TimeLeft) : Clock(Elapsed);
}

FText AScenarioGameRules::GetMessage() const
{
	if (State != EScenarioGameState::Playing)
	{
		return EndText;
	}
	return GetWorld() && GetWorld()->GetTimeSeconds() < ToastUntil ? ToastText : FText::GetEmpty();
}

FText AScenarioGameRules::GetSubMessage() const
{
	return State != EScenarioGameState::Playing ? EndSubText : FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE
