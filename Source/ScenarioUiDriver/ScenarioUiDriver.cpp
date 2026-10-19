// Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.

// Drives the editor UI from a folder of command files, so UI tests run on a
// Mac where the pointer and keyboard are busy with other work. Input goes
// through FSlateApplication like real input: hit testing, focus, capture.
//
// Off unless the editor starts with -ScenarioUiDriver=<folder>. Each command
// is <folder>/cmd-<n>.json; the answer goes to <folder>/out-<n>.json and the
// command moves to <folder>/done/. Commands ({"op": ...}):
//   dump            visible texts, hints and tags with screen rectangles
//   click           {text | tag, nth, exact, button} on the matching widget
//   at              {x, y, button}: a click at a screen position
//   type            {text} or {fifo}: characters to the focused widget
//   key             {key, shift, cmd}: one key press, for example Enter
//   scroll          {x, y, delta}
//   tab             {id}: opens a registered tab
//   windows         titles and rectangles of the open windows
//   shot            {path, widget | window}: renders a widget (by type, for
//                   example SScenarioStudio) or a window (title contains) to
//                   a PNG through Slate, so it works with the editor in the
//                   background
// Text read from {fifo} (a named pipe) never touches the disk.

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedChildren.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

#include <stdio.h>

DEFINE_LOG_CATEGORY_STATIC(LogScenarioUiDriver, Log, All);

namespace ScenarioUiDriver
{
	struct FFound
	{
		TSharedPtr<SWidget> Widget;
		FString Type;
		FString Text;
		FString Hint;
		FString Tag;
		FString Window;
		FSlateRect Rect;
	};

	void Walk(const FArrangedWidget& Node, const FString& Window, TArray<FFound>& Out)
	{
		const TSharedRef<SWidget>& W = Node.Widget;
		FFound F;
		F.Type = W->GetType().ToString();
		if (F.Type == TEXT("STextBlock"))
		{
			F.Text = StaticCastSharedRef<STextBlock>(W)->GetText().ToString();
		}
		else if (F.Type == TEXT("SEditableText"))
		{
			F.Text = StaticCastSharedRef<SEditableText>(W)->GetText().ToString();
			F.Hint = StaticCastSharedRef<SEditableText>(W)->GetHintText().ToString();
		}
		else if (F.Type == TEXT("SMultiLineEditableText"))
		{
			F.Text = StaticCastSharedRef<SMultiLineEditableText>(W)->GetText().ToString();
			F.Hint = StaticCastSharedRef<SMultiLineEditableText>(W)->GetHintText().ToString();
		}
		if (W->GetTag() != NAME_None)
		{
			F.Tag = W->GetTag().ToString();
		}
		if (const TSharedPtr<FTagMetaData> Meta = W->GetMetaData<FTagMetaData>())
		{
			F.Tag = Meta->Tag.ToString();
		}
		if (!F.Text.IsEmpty() || !F.Hint.IsEmpty() || !F.Tag.IsEmpty() || F.Type == TEXT("SEditableText") || F.Type == TEXT("SMultiLineEditableText"))
		{
			F.Widget = W;
			F.Window = Window;
			F.Rect = Node.Geometry.GetLayoutBoundingRect();
			Out.Add(F);
		}

		FArrangedChildren Children(EVisibility::Visible);
		W->ArrangeChildren(Node.Geometry, Children, true);
		for (int32 Index = 0; Index < Children.Num(); ++Index)
		{
			Walk(Children[Index], Window, Out);
		}
	}

	void AddWindow(const TSharedRef<SWindow>& Window, TArray<TSharedRef<SWindow>>& Out)
	{
		if (Window->IsVisible())
		{
			Out.Add(Window);
		}
		for (const TSharedRef<SWindow>& Child : Window->GetChildWindows())
		{
			AddWindow(Child, Out);
		}
	}

	/** Visible windows, the front-most last. */
	TArray<TSharedRef<SWindow>> Windows()
	{
		TArray<TSharedRef<SWindow>> Out;
		for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
		{
			AddWindow(Window, Out);
		}
		return Out;
	}

	TArray<FFound> Collect()
	{
		TArray<FFound> Out;
		for (const TSharedRef<SWindow>& Window : Windows())
		{
			Walk(FArrangedWidget(Window, Window->GetWindowGeometryInScreen()), Window->GetTitle().ToString(), Out);
		}
		return Out;
	}

	TSharedPtr<SWidget> FindType(const FArrangedWidget& Node, const FString& Type)
	{
		if (Node.Widget->GetType().ToString() == Type)
		{
			return Node.Widget;
		}
		FArrangedChildren Children(EVisibility::Visible);
		Node.Widget->ArrangeChildren(Node.Geometry, Children, true);
		for (int32 Index = 0; Index < Children.Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindType(Children[Index], Type))
			{
				return Found;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> ToJson(const FFound& F)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("type"), F.Type);
		if (!F.Text.IsEmpty()) { J->SetStringField(TEXT("text"), F.Text); }
		if (!F.Hint.IsEmpty()) { J->SetStringField(TEXT("hint"), F.Hint); }
		if (!F.Tag.IsEmpty()) { J->SetStringField(TEXT("tag"), F.Tag); }
		J->SetStringField(TEXT("window"), F.Window);
		TArray<TSharedPtr<FJsonValue>> R;
		for (const double V : { (double)F.Rect.Left, (double)F.Rect.Top, (double)F.Rect.Right, (double)F.Rect.Bottom })
		{
			R.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V)));
		}
		J->SetArrayField(TEXT("rect"), R);
		return J;
	}

	/**
	 * Paints every window now. macOS stops drawing an editor that is hidden
	 * behind other apps, and Slate hit-tests against the last painted frame,
	 * so a click after a page change would land on the old layout.
	 */
	void Repaint()
	{
		for (const TSharedRef<SWindow>& Window : Windows())
		{
			FSlateApplication::Get().ForceRedrawWindow(Window);
		}
	}

	void Click(const FVector2D& Pos, const FKey& Button)
	{
		FSlateApplication& App = FSlateApplication::Get();
		Repaint();
		const FModifierKeysState Mods;
		const uint32 Pointer = FSlateApplication::CursorPointerIndex;
		App.ProcessMouseMoveEvent(FPointerEvent(Pointer, Pos, Pos, TSet<FKey>(), EKeys::Invalid, 0.0f, Mods));
		App.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Pointer, Pos, Pos, TSet<FKey>({ Button }), Button, 0.0f, Mods));
		App.ProcessMouseButtonUpEvent(FPointerEvent(Pointer, Pos, Pos, TSet<FKey>(), Button, 0.0f, Mods));
	}

	FString ReadFifo(const FString& Path)
	{
		FString Out;
		if (FILE* File = fopen(TCHAR_TO_UTF8(*Path), "r"))
		{
			TArray<ANSICHAR> Bytes;
			char Buffer[256];
			size_t Read = 0;
			while ((Read = fread(Buffer, 1, sizeof(Buffer), File)) > 0)
			{
				Bytes.Append(Buffer, (int32)Read);
			}
			fclose(File);
			Bytes.Add(0);
			Out = UTF8_TO_TCHAR(Bytes.GetData());
			Out.TrimEndInline();
			FPlatformMemory::Memzero(Bytes.GetData(), Bytes.Num());
		}
		return Out;
	}

	TSharedRef<FJsonObject> Run(const TSharedPtr<FJsonObject>& Cmd)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		const FString Op = Cmd->GetStringField(TEXT("op"));
		const FKey Button = Cmd->HasField(TEXT("button")) && Cmd->GetStringField(TEXT("button")) == TEXT("right") ? EKeys::RightMouseButton : EKeys::LeftMouseButton;
		FSlateApplication& App = FSlateApplication::Get();
		Out->SetStringField(TEXT("op"), Op);
		Out->SetBoolField(TEXT("ok"), true);

		if (Op == TEXT("dump"))
		{
			Repaint();
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FFound& F : Collect())
			{
				Items.Add(MakeShared<FJsonValueObject>(ToJson(F)));
			}
			Out->SetArrayField(TEXT("items"), Items);
		}
		else if (Op == TEXT("windows"))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedRef<SWindow>& Window : Windows())
			{
				FFound F;
				F.Type = TEXT("SWindow");
				F.Text = Window->GetTitle().ToString();
				F.Rect = Window->GetRectInScreen();
				Items.Add(MakeShared<FJsonValueObject>(ToJson(F)));
			}
			Out->SetArrayField(TEXT("items"), Items);
		}
		else if (Op == TEXT("click"))
		{
			const FString Text = Cmd->HasField(TEXT("text")) ? Cmd->GetStringField(TEXT("text")) : FString();
			const FString Tag = Cmd->HasField(TEXT("tag")) ? Cmd->GetStringField(TEXT("tag")) : FString();
			const bool bExact = !Cmd->HasField(TEXT("exact")) || Cmd->GetBoolField(TEXT("exact"));
			const int32 Nth = Cmd->HasField(TEXT("nth")) ? (int32)Cmd->GetNumberField(TEXT("nth")) : 0;
			TArray<FFound> Matches;
			for (const FFound& F : Collect())
			{
				const FString& Value = !Tag.IsEmpty() ? F.Tag : (F.Text.IsEmpty() ? F.Hint : F.Text);
				const FString& Wanted = !Tag.IsEmpty() ? Tag : Text;
				if (!Wanted.IsEmpty() && (bExact ? Value == Wanted : Value.Contains(Wanted)))
				{
					Matches.Add(F);
				}
			}
			// Front-most window first.
			Algo::Reverse(Matches);
			Out->SetNumberField(TEXT("matches"), Matches.Num());
			if (!Matches.IsValidIndex(Nth))
			{
				Out->SetBoolField(TEXT("ok"), false);
				Out->SetStringField(TEXT("error"), TEXT("no such widget"));
			}
			else
			{
				const FVector2D Pos(Matches[Nth].Rect.GetCenter());
				Repaint();
				// What the click will hit, and who holds the mouse: for tests that fail to click.
				const FWidgetPath Path = App.LocateWindowUnderMouse(Pos, App.GetInteractiveTopLevelWindows(), false, FSlateApplication::CursorUserIndex);
				FString Hit;
				for (int32 Index = FMath::Max(0, Path.Widgets.Num() - 4); Index < Path.Widgets.Num(); ++Index)
				{
					Hit += Path.Widgets[Index].Widget->GetType().ToString() + TEXT(" ");
				}
				Out->SetStringField(TEXT("hit"), Hit.TrimEnd());
				Out->SetBoolField(TEXT("captured"), App.HasAnyMouseCaptor());
				Click(Pos, Button);
				Out->SetObjectField(TEXT("clicked"), ToJson(Matches[Nth]));
			}
		}
		else if (Op == TEXT("at"))
		{
			Click(FVector2D(Cmd->GetNumberField(TEXT("x")), Cmd->GetNumberField(TEXT("y"))), Button);
		}
		else if (Op == TEXT("type"))
		{
			FString Text = Cmd->HasField(TEXT("fifo")) ? ReadFifo(Cmd->GetStringField(TEXT("fifo"))) : Cmd->GetStringField(TEXT("text"));
			for (const TCHAR Char : Text)
			{
				App.ProcessKeyCharEvent(FCharacterEvent(Char, FModifierKeysState(), 0, false));
			}
			Out->SetNumberField(TEXT("chars"), Text.Len());
			FPlatformMemory::Memzero(Text.GetCharArray().GetData(), Text.GetCharArray().Num() * sizeof(TCHAR));
		}
		else if (Op == TEXT("key"))
		{
			const FKey Key(*Cmd->GetStringField(TEXT("key")));
			const bool bShift = Cmd->HasField(TEXT("shift")) && Cmd->GetBoolField(TEXT("shift"));
			const bool bCmd = Cmd->HasField(TEXT("cmd")) && Cmd->GetBoolField(TEXT("cmd"));
			const FModifierKeysState Mods(bShift, false, false, false, false, false, bCmd, false, false);
			App.ProcessKeyDownEvent(FKeyEvent(Key, Mods, 0, false, 0, 0));
			App.ProcessKeyUpEvent(FKeyEvent(Key, Mods, 0, false, 0, 0));
		}
		else if (Op == TEXT("scroll"))
		{
			Repaint();
			const FVector2D Pos(Cmd->GetNumberField(TEXT("x")), Cmd->GetNumberField(TEXT("y")));
			App.ProcessMouseMoveEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Pos, Pos, TSet<FKey>(), EKeys::Invalid, 0.0f, FModifierKeysState()));
			App.ProcessMouseWheelOrGestureEvent(FPointerEvent(FSlateApplication::CursorPointerIndex, Pos, Pos, TSet<FKey>(), EKeys::Invalid, (float)Cmd->GetNumberField(TEXT("delta")), FModifierKeysState()), nullptr);
		}
		else if (Op == TEXT("shot"))
		{
			const FString Type = Cmd->HasField(TEXT("widget")) ? Cmd->GetStringField(TEXT("widget")) : FString();
			const FString Title = Cmd->HasField(TEXT("window")) ? Cmd->GetStringField(TEXT("window")) : FString();
			TSharedPtr<SWidget> Target;
			TArray<TSharedRef<SWindow>> All = Windows();
			Algo::Reverse(All);
			for (const TSharedRef<SWindow>& Window : All)
			{
				if (!Type.IsEmpty())
				{
					Target = FindType(FArrangedWidget(Window, Window->GetWindowGeometryInScreen()), Type);
				}
				else if (Window->GetTitle().ToString().Contains(Title))
				{
					Target = Window;
				}
				if (Target.IsValid())
				{
					break;
				}
			}
			TArray<FColor> Pixels;
			FIntVector Size;
			if (!Target.IsValid() || !App.TakeScreenshot(Target.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
			{
				Out->SetBoolField(TEXT("ok"), false);
				Out->SetStringField(TEXT("error"), Target.IsValid() ? TEXT("screenshot failed") : TEXT("no such widget"));
			}
			else
			{
				for (FColor& Pixel : Pixels)
				{
					Pixel.A = 255;
				}
				TArray64<uint8> Png;
				FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
				Out->SetBoolField(TEXT("ok"), FFileHelper::SaveArrayToFile(Png, *Cmd->GetStringField(TEXT("path"))));
				Out->SetNumberField(TEXT("width"), Size.X);
				Out->SetNumberField(TEXT("height"), Size.Y);
			}
		}
		else if (Op == TEXT("tab"))
		{
			Out->SetBoolField(TEXT("ok"), FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FName(*Cmd->GetStringField(TEXT("id"))))).IsValid());
		}
		else
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), TEXT("unknown op"));
		}
		return Out;
	}
}

class FScenarioUiDriverModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("-ScenarioUiDriver="), Folder) || Folder.IsEmpty())
		{
			return;
		}
		IFileManager::Get().MakeDirectory(*(Folder / TEXT("done")), true);
		UE_LOG(LogScenarioUiDriver, Display, TEXT("UI driver reading commands from %s."), *Folder);
		Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FScenarioUiDriverModule::Tick), 0.1f);
	}

	virtual void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	}

private:
	bool Tick(float)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return true;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Folder / TEXT("cmd-*.json")), true, false);
		Files.Sort();
		if (Files.Num() == 0)
		{
			return true;
		}
		const FString Name = Files[0];
		const FString Path = Folder / Name;
		FString Body;
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (FFileHelper::LoadFileToString(Body, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Cmd) && Cmd.IsValid())
		{
			Out = ScenarioUiDriver::Run(Cmd);
		}
		else
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), TEXT("unreadable command"));
		}
		IFileManager::Get().Move(*(Folder / TEXT("done") / Name), *Path, true);
		FString Text;
		FJsonSerializer::Serialize(Out, TJsonWriterFactory<>::Create(&Text));
		const FString Answer = Folder / Name.Replace(TEXT("cmd-"), TEXT("out-"));
		FFileHelper::SaveStringToFile(Text, *(Answer + TEXT(".tmp")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		IFileManager::Get().Move(*Answer, *(Answer + TEXT(".tmp")), true);
		return true;
	}

	FString Folder;
	FTSTicker::FDelegateHandle Ticker;
};

IMPLEMENT_MODULE(FScenarioUiDriverModule, ScenarioUiDriver);
