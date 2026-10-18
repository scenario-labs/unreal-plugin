// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

#include "ScenarioFiles.h"

#include "Misc/DateTime.h"

namespace
{
	bool HeadIs(const TArray<uint8>& Head, int32 Start, const ANSICHAR* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text);
		if (Head.Num() < Start + Length)
		{
			return false;
		}
		return FMemory::Memcmp(Head.GetData() + Start, Text, Length) == 0;
	}

	bool IsStopWord(const FString& Word)
	{
		static const TSet<FString> Words = {
			TEXT("a"), TEXT("an"), TEXT("the"), TEXT("of"), TEXT("with"), TEXT("and"), TEXT("in"), TEXT("on"), TEXT("for"),
			TEXT("to"), TEXT("at"), TEXT("by"), TEXT("from"), TEXT("scenario"), TEXT("very"), TEXT("some"),
		};
		return Words.Contains(Word);
	}
}

FString ScenarioFiles::RoleFor(const FString& MetadataType)
{
	static const TMap<FString, FString> Roles = {
		{ TEXT("texture-albedo"), TEXT("albedo") },
		{ TEXT("texture-basecolor"), TEXT("albedo") },
		{ TEXT("texture-base-color"), TEXT("albedo") },
		{ TEXT("texture-normal"), TEXT("normal") },
		{ TEXT("texture-smoothness"), TEXT("roughness") },
		{ TEXT("texture-roughness"), TEXT("roughness") },
		{ TEXT("texture-metallic"), TEXT("metallic") },
		{ TEXT("texture-metalness"), TEXT("metallic") },
		{ TEXT("texture-height"), TEXT("height") },
		{ TEXT("texture-ao"), TEXT("ao") },
		{ TEXT("texture-occlusion"), TEXT("ao") },
		{ TEXT("inference-txt2img-texture"), TEXT("base") },
		{ TEXT("skybox-base-360"), TEXT("panorama") },
	};
	const FString* Found = Roles.Find(MetadataType.ToLower());
	return Found ? *Found : TEXT("main");
}

FString ScenarioFiles::RoleSuffix(const FString& Role)
{
	if (Role == TEXT("albedo") || Role == TEXT("base"))
	{
		return TEXT("BC");
	}
	if (Role == TEXT("normal"))
	{
		return TEXT("N");
	}
	if (Role == TEXT("roughness"))
	{
		return TEXT("R");
	}
	if (Role == TEXT("metallic"))
	{
		return TEXT("M");
	}
	if (Role == TEXT("height"))
	{
		return TEXT("H");
	}
	if (Role == TEXT("ao"))
	{
		return TEXT("AO");
	}
	return FString();
}

FString ScenarioFiles::Slug(const FString& Text, int32 MaxLength)
{
	FString Out;
	bool bDash = false;
	for (const TCHAR Char : Text.ToLower())
	{
		const bool bKeep = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9'));
		if (bKeep)
		{
			if (bDash && !Out.IsEmpty())
			{
				Out += TEXT('-');
			}
			Out += Char;
			bDash = false;
		}
		else
		{
			bDash = true;
		}
		if (Out.Len() >= MaxLength)
		{
			break;
		}
	}
	Out = Out.Left(MaxLength);
	while (Out.EndsWith(TEXT("-")))
	{
		Out.LeftChopInline(1);
	}
	return Out.IsEmpty() ? TEXT("scenario") : Out;
}

FString ScenarioFiles::PascalSlug(const FString& Text, int32 MaxWords)
{
	TArray<FString> Words;
	Slug(Text, 120).ParseIntoArray(Words, TEXT("-"));
	FString Out;
	int32 Count = 0;
	for (const FString& Word : Words)
	{
		if (IsStopWord(Word))
		{
			continue;
		}
		Out += Word.Left(1).ToUpper() + Word.Mid(1);
		if (++Count >= MaxWords)
		{
			break;
		}
	}
	// A name cannot start with a digit in some tools; prefix it.
	if (!Out.IsEmpty() && FChar::IsDigit(Out[0]))
	{
		Out = TEXT("N") + Out;
	}
	return Out.IsEmpty() ? TEXT("Scenario") : Out;
}

FString ScenarioFiles::Tail(const FString& AssetId)
{
	FString Id = AssetId;
	Id.RemoveFromStart(TEXT("asset_"));
	FString Clean;
	for (const TCHAR Char : Id)
	{
		if (FChar::IsAlnum(Char) && Char < 128)
		{
			Clean += Char;
		}
	}
	return Clean.Right(6);
}

FString ScenarioFiles::AssetName(const FString& Prefix, const FString& Prompt, const FString& AssetId, const FString& Role)
{
	const FString Suffix = RoleSuffix(Role);
	const FString TailText = Tail(AssetId);
	// Prefix, tail and suffix are short; the slug gets what is left of 63 characters.
	const int32 Fixed = Prefix.Len() + 1 + (TailText.IsEmpty() ? 0 : TailText.Len() + 1) + (Suffix.IsEmpty() ? 0 : Suffix.Len() + 1);
	FString SlugText = PascalSlug(Prompt).Left(FMath::Max(8, 63 - Fixed));
	FString Name = Prefix + TEXT("_") + SlugText;
	if (!TailText.IsEmpty())
	{
		Name += TEXT("_") + TailText;
	}
	if (!Suffix.IsEmpty())
	{
		Name += TEXT("_") + Suffix;
	}
	return SafeObjectName(Name);
}

FString ScenarioFiles::SniffExtension(const TArray<uint8>& Head)
{
	if (Head.Num() >= 8 && Head[0] == 0x89 && Head[1] == 0x50 && Head[2] == 0x4E && Head[3] == 0x47)
	{
		return TEXT("png");
	}
	if (Head.Num() >= 3 && Head[0] == 0xFF && Head[1] == 0xD8 && Head[2] == 0xFF)
	{
		return TEXT("jpg");
	}
	if (HeadIs(Head, 0, "RIFF"))
	{
		if (HeadIs(Head, 8, "WEBP"))
		{
			return TEXT("webp");
		}
		if (HeadIs(Head, 8, "WAVE"))
		{
			return TEXT("wav");
		}
	}
	if (HeadIs(Head, 0, "glTF"))
	{
		return TEXT("glb");
	}
	if (HeadIs(Head, 0, "OggS"))
	{
		return TEXT("ogg");
	}
	if (HeadIs(Head, 0, "fLaC"))
	{
		return TEXT("flac");
	}
	if (HeadIs(Head, 0, "ID3"))
	{
		return TEXT("mp3");
	}
	if (HeadIs(Head, 4, "ftyp"))
	{
		// ISO base media: MP4 and MOV share the box layout.
		return HeadIs(Head, 8, "qt  ") ? TEXT("mov") : TEXT("mp4");
	}
	if (Head.Num() >= 4 && Head[0] == 0x1A && Head[1] == 0x45 && Head[2] == 0xDF && Head[3] == 0xA3)
	{
		return TEXT("webm");
	}
	if (Head.Num() >= 4 && Head[0] == 0x76 && Head[1] == 0x2F && Head[2] == 0x31 && Head[3] == 0x01)
	{
		return TEXT("exr");
	}
	if (HeadIs(Head, 0, "#?RADIANCE") || HeadIs(Head, 0, "#?RGBE"))
	{
		return TEXT("hdr");
	}
	if (HeadIs(Head, 0, "Kaydara FBX Binary"))
	{
		return TEXT("fbx");
	}
	if (Head.Num() >= 2 && Head[0] == 0xFF && (Head[1] & 0xE0) == 0xE0)
	{
		return TEXT("mp3");
	}
	if (Head.Num() >= 1 && Head[0] == 0x7B)
	{
		return TEXT("gltf");
	}
	return FString();
}

FString ScenarioFiles::ExtensionForMime(const FString& Mime)
{
	FString Clean = Mime.ToLower();
	FString Left;
	if (Clean.Split(TEXT(";"), &Left, nullptr))
	{
		Clean = Left;
	}
	Clean.TrimStartAndEndInline();
	static const TMap<FString, FString> Map = {
		{ TEXT("image/png"), TEXT("png") },
		{ TEXT("image/jpeg"), TEXT("jpg") },
		{ TEXT("image/jpg"), TEXT("jpg") },
		{ TEXT("image/webp"), TEXT("webp") },
		{ TEXT("image/x-exr"), TEXT("exr") },
		{ TEXT("image/aces"), TEXT("exr") },
		{ TEXT("image/vnd.radiance"), TEXT("hdr") },
		{ TEXT("image/svg+xml"), TEXT("svg") },
		{ TEXT("model/gltf-binary"), TEXT("glb") },
		{ TEXT("model/gltf+json"), TEXT("gltf") },
		{ TEXT("model/x-fbx"), TEXT("fbx") },
		{ TEXT("model/obj"), TEXT("obj") },
		{ TEXT("audio/mpeg"), TEXT("mp3") },
		{ TEXT("audio/mp3"), TEXT("mp3") },
		{ TEXT("audio/wav"), TEXT("wav") },
		{ TEXT("audio/x-wav"), TEXT("wav") },
		{ TEXT("audio/wave"), TEXT("wav") },
		{ TEXT("audio/ogg"), TEXT("ogg") },
		{ TEXT("audio/flac"), TEXT("flac") },
		{ TEXT("video/mp4"), TEXT("mp4") },
		{ TEXT("video/quicktime"), TEXT("mov") },
		{ TEXT("video/webm"), TEXT("webm") },
	};
	const FString* Found = Map.Find(Clean);
	return Found ? *Found : FString();
}

FString ScenarioFiles::MimeForExtension(const FString& Extension)
{
	static const TMap<FString, FString> Map = {
		{ TEXT("png"), TEXT("image/png") },
		{ TEXT("jpg"), TEXT("image/jpeg") },
		{ TEXT("jpeg"), TEXT("image/jpeg") },
		{ TEXT("webp"), TEXT("image/webp") },
		{ TEXT("exr"), TEXT("image/x-exr") },
		{ TEXT("hdr"), TEXT("image/vnd.radiance") },
		{ TEXT("glb"), TEXT("model/gltf-binary") },
		{ TEXT("gltf"), TEXT("model/gltf+json") },
		{ TEXT("fbx"), TEXT("model/x-fbx") },
		{ TEXT("obj"), TEXT("model/obj") },
		{ TEXT("wav"), TEXT("audio/wav") },
		{ TEXT("mp3"), TEXT("audio/mpeg") },
		{ TEXT("ogg"), TEXT("audio/ogg") },
		{ TEXT("flac"), TEXT("audio/flac") },
		{ TEXT("mp4"), TEXT("video/mp4") },
		{ TEXT("mov"), TEXT("video/quicktime") },
		{ TEXT("webm"), TEXT("video/webm") },
	};
	const FString* Found = Map.Find(Extension.ToLower());
	return Found ? *Found : FString();
}

FString ScenarioFiles::UploadKindForExtension(const FString& Extension)
{
	const FString Ext = Extension.ToLower();
	if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("webp"))
	{
		return TEXT("image");
	}
	if (Ext == TEXT("glb") || Ext == TEXT("gltf") || Ext == TEXT("fbx") || Ext == TEXT("obj"))
	{
		return TEXT("3d");
	}
	if (Ext == TEXT("wav") || Ext == TEXT("mp3") || Ext == TEXT("ogg") || Ext == TEXT("flac"))
	{
		return TEXT("audio");
	}
	if (Ext == TEXT("mp4") || Ext == TEXT("mov") || Ext == TEXT("webm"))
	{
		return TEXT("video");
	}
	return FString();
}

FString ScenarioFiles::KindFolder(const FString& Lane)
{
	static const TMap<FString, FString> Folders = {
		{ TEXT("image"), TEXT("Images") },
		{ TEXT("video"), TEXT("Video") },
		{ TEXT("3d"), TEXT("Meshes") },
		{ TEXT("3d_edit"), TEXT("Meshes") },
		{ TEXT("character"), TEXT("Characters") },
		{ TEXT("material"), TEXT("Materials") },
		{ TEXT("audio"), TEXT("Audio") },
		{ TEXT("environment"), TEXT("Environments") },
		{ TEXT("render"), TEXT("Renders") },
		{ TEXT("workflow"), TEXT("Workflows") },
		{ TEXT("library"), TEXT("Library") },
	};
	const FString* Found = Folders.Find(Lane);
	return Found ? *Found : TEXT("Generated");
}

FString ScenarioFiles::Today()
{
	const FDateTime Now = FDateTime::Now();
	return FString::Printf(TEXT("%04d-%02d-%02d"), Now.GetYear(), Now.GetMonth(), Now.GetDay());
}

FString ScenarioFiles::LocalDate(double UnixTime)
{
	const FDateTime Local = FDateTime::FromUnixTimestamp(static_cast<int64>(UnixTime)) + (FDateTime::Now() - FDateTime::UtcNow());
	return FString::Printf(TEXT("%04d-%02d-%02d"), Local.GetYear(), Local.GetMonth(), Local.GetDay());
}

FString ScenarioFiles::SafeObjectName(const FString& Name)
{
	FString Out;
	for (const TCHAR Char : Name)
	{
		Out += (FChar::IsAlnum(Char) && Char < 128) || Char == TEXT('_') ? Char : TEXT('_');
	}
	while (Out.Contains(TEXT("__")))
	{
		Out.ReplaceInline(TEXT("__"), TEXT("_"));
	}
	return Out.Left(63);
}
