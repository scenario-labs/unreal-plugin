// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** File naming, type sniffing and output roles for Scenario results. */
namespace ScenarioFiles
{
	/**
	 * Scenario asset metadata.type to the role the importer uses: albedo, normal,
	 * roughness, metallic, height, ao, base, panorama, or main.
	 * PATINA labels its roughness map "texture-smoothness": the pixels are
	 * roughness, so it goes to the roughness slot without inversion.
	 */
	SCENARIOCORE_API FString RoleFor(const FString& MetadataType);

	/** Role to the name suffix: BC, N, R, M, H, AO; empty for main. */
	SCENARIOCORE_API FString RoleSuffix(const FString& Role);

	/** Lowercase words joined by dashes, at most MaxLength characters ("scenario" when empty). */
	SCENARIOCORE_API FString Slug(const FString& Text, int32 MaxLength = 40);

	/** Up to MaxWords meaningful words in PascalCase, ASCII only: "a weathered pirate chest" -> "WeatheredPirateChest". */
	SCENARIOCORE_API FString PascalSlug(const FString& Text, int32 MaxWords = 5);

	/** Last six characters of the asset id, without the "asset_" prefix. */
	SCENARIOCORE_API FString Tail(const FString& AssetId);

	/** "<Prefix>_<Slug>_<Tail>[_<Role>]", kept under 64 characters. */
	SCENARIOCORE_API FString AssetName(const FString& Prefix, const FString& Prompt, const FString& AssetId, const FString& Role = FString());

	/** Extension from the first bytes, or empty when unknown. */
	SCENARIOCORE_API FString SniffExtension(const TArray<uint8>& Head);
	SCENARIOCORE_API FString ExtensionForMime(const FString& Mime);
	SCENARIOCORE_API FString MimeForExtension(const FString& Extension);
	/** Scenario asset kind for an upload: image, video, audio or 3d (empty when not uploadable). */
	SCENARIOCORE_API FString UploadKindForExtension(const FString& Extension);

	/** Content folder under the Scenario root for a lane: Images, Meshes, Materials... */
	SCENARIOCORE_API FString KindFolder(const FString& Lane);

	/** Today's local date, "2026-10-18". */
	SCENARIOCORE_API FString Today();

	/** The local date of a Unix time, "2026-10-18": date folders follow the user's clock, not UTC. */
	SCENARIOCORE_API FString LocalDate(double UnixTime);

	/** Keeps letters, digits and underscores, so the name is a valid Unreal object name. */
	SCENARIOCORE_API FString SafeObjectName(const FString& Name);
}
