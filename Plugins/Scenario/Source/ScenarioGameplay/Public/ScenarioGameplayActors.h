// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ScenarioGameplayActors.generated.h"

class UAudioComponent;
class UBillboardComponent;
class UBoxComponent;
class UPointLightComponent;
class USoundBase;
class USphereComponent;
class UStaticMeshComponent;
class SWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FScenarioGameEvent);

UENUM(BlueprintType)
enum class EScenarioGameState : uint8
{
	Playing,
	Won,
	Lost,
};

/**
 * Something the player collects by walking into it. It spins, bobs and glows; the level's
 * Scenario Game Rules count it. Set the look with the Mesh component (any static mesh).
 */
UCLASS(Blueprintable, ClassGroup = Scenario, meta = (DisplayName = "Scenario Pickup"))
class SCENARIOGAMEPLAY_API AScenarioPickup : public AActor
{
	GENERATED_BODY()

public:
	AScenarioPickup();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<USphereComponent> Trigger;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<UPointLightComponent> Glow;

	/** Degrees per second around the vertical axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	float SpinDegreesPerSecond = 90.0f;

	/** Up and down motion, in centimeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	float BobHeight = 15.0f;

	/** Played where the pickup was collected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	TObjectPtr<USoundBase> PickupSound;

	UPROPERTY(BlueprintAssignable, Category = "Scenario")
	FScenarioGameEvent OnCollected;

	UFUNCTION(BlueprintPure, Category = "Scenario")
	bool IsCollected() const { return bCollected; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UFUNCTION()
	void HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	FVector MeshBase = FVector::ZeroVector;
	float Age = 0.0f;
	bool bCollected = false;
};

/**
 * Where the player ends the level. With Needs All Pickups on, it opens once every pickup is
 * collected; walking into it then wins the game.
 */
UCLASS(Blueprintable, ClassGroup = Scenario, meta = (DisplayName = "Scenario Goal"))
class SCENARIOGAMEPLAY_API AScenarioGoal : public AActor
{
	GENERATED_BODY()

public:
	AScenarioGoal();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<UBoxComponent> Trigger;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scenario")
	TObjectPtr<UPointLightComponent> Glow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	bool bNeedsAllPickups = true;

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};

/**
 * The rules of a small game, placed once in the level: counts the pickups, runs the timer,
 * plays the music and draws the HUD. Without it, pickups and goals do nothing.
 */
UCLASS(Blueprintable, ClassGroup = Scenario, meta = (DisplayName = "Scenario Game Rules"))
class SCENARIOGAMEPLAY_API AScenarioGameRules : public AActor
{
	GENERATED_BODY()

public:
	AScenarioGameRules();

	/** Shown at the top left of the screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	FText Title;

	/** One line under the title. Empty: "Collect every <pickups>, then reach the goal." */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	FText Objective;

	/** What the pickups are called, in the plural ("lanterns"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	FText PickupName;

	/** Seconds to finish. 0 means no time limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario", meta = (ClampMin = "0"))
	float TimeLimitSeconds = 180.0f;

	/** Loops for the whole game. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	TObjectPtr<USoundBase> Music;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario", meta = (ClampMin = "0", ClampMax = "2"))
	float MusicVolume = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	TObjectPtr<USoundBase> WinSound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	TObjectPtr<USoundBase> LoseSound;

	/** Seconds before the level restarts after a loss. 0 keeps the loss on screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario", meta = (ClampMin = "0"))
	float RestartDelaySeconds = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scenario")
	FLinearColor AccentColor = FLinearColor(1.0f, 0.78f, 0.3f);

	UPROPERTY(BlueprintAssignable, Category = "Scenario")
	FScenarioGameEvent OnWin;

	UPROPERTY(BlueprintAssignable, Category = "Scenario")
	FScenarioGameEvent OnLose;

	UFUNCTION(BlueprintPure, Category = "Scenario")
	int32 GetCollected() const { return Collected; }

	UFUNCTION(BlueprintPure, Category = "Scenario")
	int32 GetTotal() const { return Total; }

	UFUNCTION(BlueprintPure, Category = "Scenario")
	float GetTimeLeft() const { return TimeLeft; }

	UFUNCTION(BlueprintPure, Category = "Scenario")
	EScenarioGameState GetGameState() const { return State; }

	/** The level's rules, or null. */
	static AScenarioGameRules* Find(const UObject* WorldContext);

	void NotifyPickup(AScenarioPickup* Pickup);
	void NotifyGoal(AScenarioGoal* Goal);

	// Read by the HUD.
	FText GetObjectiveText() const;
	FText GetProgressText() const;
	FText GetTimerText() const;
	FText GetMessage() const;
	FText GetSubMessage() const;
	bool IsTimeShort() const { return TimeLimitSeconds > 0.0f && TimeLeft < 20.0f && State == EScenarioGameState::Playing; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void Toast(const FText& Text, float Seconds = 2.5f);
	void Win();
	void Lose();
	bool HasGoal() const;

	UFUNCTION()
	void LoopMusic();

	UPROPERTY(VisibleAnywhere, Category = "Scenario")
	TObjectPtr<UBillboardComponent> Sprite;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MusicComponent;

	TSharedPtr<SWidget> Hud;
	EScenarioGameState State = EScenarioGameState::Playing;
	int32 Collected = 0;
	int32 Total = 0;
	float TimeLeft = 0.0f;
	float Elapsed = 0.0f;
	FText ToastText;
	float ToastUntil = 0.0f;
	FText EndText;
	FText EndSubText;
};
