// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#pragma once

// Fixtures shared by the editor tests: files written on the fly, so no test needs a binary fixture or the network.

#include "CoreMinimal.h"
#include "Engine/Texture.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"

namespace ScenarioEditorTests
{
	inline const TCHAR* TestFolder = TEXT("/Game/ScenarioTests");

	inline FString TestDir()
	{
		return FPaths::ProjectSavedDir() / TEXT("ScenarioTests");
	}

	/** A small RGBA PNG written to disk, so imports run without network or fixtures. */
	inline bool WritePng(const FString& Path, int32 Size)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.SetNum(Size * Size);
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			Pixels[Index] = FColor(static_cast<uint8>(Index % 255), 128, 64, 255);
		}
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Bytes = Wrapper->GetCompressed();
		return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), static_cast<int32>(Bytes.Num())), *Path);
	}

	/** PNG bytes of a Size x Size image of one color. */
	inline TArray64<uint8> PngOf(const FColor& Color, int32 Size)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.Init(Color, Size * Size);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8))
		{
			return {};
		}
		return Wrapper->GetCompressed();
	}

	/**
	 * A 1 m glTF cube with base color, metallic-roughness and normal maps, the
	 * shape of a 3D model from Scenario, written here so the test needs no file.
	 */
	inline bool WriteGlb(const FString& Path)
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<FVector2f> Uvs;
		TArray<uint16> Indices;
		const FVector3f Faces[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		const FVector2f Corners[4] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		for (const FVector3f& Normal : Faces)
		{
			// Tangent x Bitangent = Normal, so the corners run counterclockwise seen from outside.
			const FVector3f Tangent = FMath::Abs(Normal.Z) > 0.5f ? FVector3f(1, 0, 0) : FVector3f(0, 0, 1);
			const FVector3f Bitangent = FVector3f::CrossProduct(Normal, Tangent);
			const uint16 First = static_cast<uint16>(Positions.Num());
			for (const FVector2f& Corner : Corners)
			{
				Positions.Add((Normal + Tangent * Corner.X + Bitangent * Corner.Y) * 0.5f);
				Normals.Add(Normal);
				Uvs.Add(FVector2f((Corner.X + 1.0f) * 0.5f, (Corner.Y + 1.0f) * 0.5f));
			}
			for (const int32 Offset : { 0, 1, 2, 0, 2, 3 })
			{
				Indices.Add(static_cast<uint16>(First + Offset));
			}
		}

		TArray<uint8> Bin;
		TArray<FString> Views;
		auto AddView = [&Bin, &Views](const void* Data, int64 Size)
		{
			Views.Add(FString::Printf(TEXT("{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%lld}"), Bin.Num(), Size));
			Bin.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			while (Bin.Num() % 4 != 0)
			{
				Bin.Add(0);
			}
		};
		AddView(Positions.GetData(), Positions.Num() * sizeof(FVector3f));
		AddView(Normals.GetData(), Normals.Num() * sizeof(FVector3f));
		AddView(Uvs.GetData(), Uvs.Num() * sizeof(FVector2f));
		AddView(Indices.GetData(), Indices.Num() * sizeof(uint16));
		// Base color, metallic-roughness (G roughness, B metallic) and a flat normal.
		for (const FColor& Color : { FColor(180, 120, 60), FColor(0, 200, 30), FColor(128, 128, 255) })
		{
			const TArray64<uint8> Png = PngOf(Color, 16);
			if (Png.Num() == 0)
			{
				return false;
			}
			AddView(Png.GetData(), Png.Num());
		}

		FString Json = FString::Printf(TEXT("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],")
			TEXT("\"nodes\":[{\"mesh\":0,\"name\":\"ScenarioTestCube\"}],")
			TEXT("\"meshes\":[{\"name\":\"ScenarioTestCube\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],")
			TEXT("\"materials\":[{\"name\":\"ScenarioTestCubeMaterial\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":1}},\"normalTexture\":{\"index\":2}}],")
			TEXT("\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2}],")
			TEXT("\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"},{\"bufferView\":5,\"mimeType\":\"image/png\"},{\"bufferView\":6,\"mimeType\":\"image/png\"}],")
			TEXT("\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},")
			TEXT("{\"bufferView\":1,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":%d,\"type\":\"VEC2\"},")
			TEXT("{\"bufferView\":3,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}],")
			TEXT("\"bufferViews\":[%s],\"buffers\":[{\"byteLength\":%d}]}"),
			Positions.Num(), Normals.Num(), Uvs.Num(), Indices.Num(), *FString::Join(Views, TEXT(",")), Bin.Num());
		FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> JsonBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		while (JsonBytes.Num() % 4 != 0)
		{
			JsonBytes.Add(' ');
		}

		TArray<uint8> Glb;
		auto Word = [&Glb](uint32 Value) { Glb.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		Word(0x46546C67); // "glTF"
		Word(2);
		Word(12 + 8 + JsonBytes.Num() + 8 + Bin.Num());
		Word(JsonBytes.Num());
		Word(0x4E4F534A); // "JSON"
		Glb.Append(JsonBytes);
		Word(Bin.Num());
		Word(0x004E4942); // "BIN"
		Glb.Append(Bin);
		return FFileHelper::SaveArrayToFile(Glb, *Path);
	}

	/**
	 * Force-deletes test assets, textures last: deleting a texture first nulls
	 * it in the materials that sample it, which then fail to recompile.
	 */
	inline void DeleteAssets(const TArray<UObject*>& Objects)
	{
		// Sorted before any delete: a force delete collects garbage, so the
		// pointers are not read again afterwards.
		TArray<UObject*> Others;
		TArray<UObject*> Textures;
		for (UObject* Object : Objects)
		{
			if (IsValid(Object))
			{
				(Object->IsA<UTexture>() ? Textures : Others).AddUnique(Object);
			}
		}
		for (TArray<UObject*>* Batch : { &Others, &Textures })
		{
			if (Batch->Num() > 0)
			{
				ObjectTools::ForceDeleteObjects(*Batch, false);
			}
		}
	}
}

#endif
