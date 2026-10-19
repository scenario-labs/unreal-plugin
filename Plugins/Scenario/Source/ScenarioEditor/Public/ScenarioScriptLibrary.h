// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ScenarioScriptLibrary.generated.h"

class UTexture2D;

/** State of an action started from a script. */
USTRUCT(BlueprintType)
struct SCENARIOEDITOR_API FScenarioScriptCall
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Scenario")
	FString CallId;

	UPROPERTY(BlueprintReadOnly, Category = "Scenario")
	bool bDone = false;

	UPROPERTY(BlueprintReadOnly, Category = "Scenario")
	bool bOk = false;

	/** "Type: message" when the action failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Scenario")
	FString Error;

	/** The action's result as JSON, the same as an agent receives. */
	UPROPERTY(BlueprintReadOnly, Category = "Scenario")
	FString ResultJson;
};

/**
 * Scenario for Editor Utility Blueprints and Unreal Python. Every action has
 * the same name, arguments and money rules as the MCP tools: paid actions need
 * a quote_id and approved_cost from an estimate. Scripts may import, place and
 * save; they never run agent Python.
 *
 * Calls are asynchronous: start one, then poll it from a tick callback
 * (unreal.register_slate_post_tick_callback in Python). Blocking the game
 * thread while waiting stops the network replies.
 */
UCLASS()
class SCENARIOEDITOR_API UScenarioScriptLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Starts an action. ArgumentsJson is a JSON object ("{}" for none). Returns the call id. */
	UFUNCTION(BlueprintCallable, Category = "Scenario|Scripting")
	static FString StartAction(const FString& Name, const FString& ArgumentsJson, const FString& ScriptName);

	/** The call's state; the last 200 calls are kept. */
	UFUNCTION(BlueprintCallable, Category = "Scenario|Scripting")
	static FScenarioScriptCall GetCall(const FString& CallId);

	UFUNCTION(BlueprintPure, Category = "Scenario|Scripting")
	static TArray<FString> ListActions();

	/** Description and argument schema of one action, as JSON. */
	UFUNCTION(BlueprintPure, Category = "Scenario|Scripting")
	static FString DescribeAction(const FString& Name);

	/** Signed in with a project chosen. */
	UFUNCTION(BlueprintPure, Category = "Scenario|Scripting")
	static bool IsReady();

	/**
	 * The sun in an equirectangular panorama: the brightest region of its upper
	 * half. Azimuth and elevation are in degrees, in the Scenario sky sphere's
	 * mapping: azimuth 0 looks along world +X and 90 along +Y, elevation 90 is
	 * straight up. Strength is that region's brightness over the sky's median:
	 * about 3 or more is a clear sun, near 1 an overcast sky. A directional
	 * light lit from there has pitch -Elevation and yaw Azimuth + 180.
	 * False when the texture has no source image.
	 * Python returns (azimuth, elevation, strength), or None when it is false:
	 * found = unreal.ScenarioScriptLibrary.find_panorama_sun(texture)
	 */
	UFUNCTION(BlueprintCallable, Category = "Scenario|Scripting")
	static bool FindPanoramaSun(UTexture2D* Panorama, float& AzimuthDegrees, float& ElevationDegrees, float& Strength);
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnScenarioActionFinished, bool, bOk, const FString&, ResultJson, const FString&, Error);

/** "Run Scenario Action": the asynchronous node for Editor Utility Blueprints. */
UCLASS()
class SCENARIOEDITOR_API UScenarioRunActionAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FOnScenarioActionFinished OnFinished;

	UFUNCTION(BlueprintCallable, Category = "Scenario|Scripting", meta = (BlueprintInternalUseOnly = "true", DisplayName = "Run Scenario Action"))
	static UScenarioRunActionAsync* RunScenarioAction(const FString& Name, const FString& ArgumentsJson);

	virtual void Activate() override;

private:
	FString ActionName;
	FString Arguments;
};
