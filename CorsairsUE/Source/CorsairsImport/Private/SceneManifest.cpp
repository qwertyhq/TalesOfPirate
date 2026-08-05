#include "SceneManifest.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsManifest, Log, All);

namespace
{

// Читает целое поле, отсутствие которого делает запись бессмысленной.
bool ReadRequiredInt(const TSharedPtr<FJsonObject>& Object, const FString& Field,
					 int32& OutValue, FString& OutError)
{
	if (!Object->TryGetNumberField(Field, OutValue))
	{
		OutError = FString::Printf(TEXT("нет обязательного поля '%s'"), *Field);
		return false;
	}
	return true;
}

} // namespace

bool UCorsairsSceneManifestLibrary::LoadSceneManifest(const FString& FilePath,
													  FCorsairsSceneManifest& OutManifest,
													  FString& OutError)
{
	OutManifest = FCorsairsSceneManifest{};
	OutError.Empty();

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *FilePath))
	{
		OutError = FString::Printf(TEXT("не удалось прочитать %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("файл не является корректным JSON");
		return false;
	}

	if (!ReadRequiredInt(Root, TEXT("sectionCntX"), OutManifest.SectionCntX, OutError) ||
		!ReadRequiredInt(Root, TEXT("sectionCntY"), OutManifest.SectionCntY, OutError) ||
		!ReadRequiredInt(Root, TEXT("sectionWidth"), OutManifest.SectionWidth, OutError) ||
		!ReadRequiredInt(Root, TEXT("sectionHeight"), OutManifest.SectionHeight, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objects))
	{
		OutError = TEXT("нет массива 'objects'");
		return false;
	}

	OutManifest.Objects.Reserve(Objects->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Objects)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		if (!Value->TryGetObject(Item))
		{
			OutError = TEXT("элемент 'objects' не является объектом");
			return false;
		}

		FCorsairsPlacedObject Placed;
		if (!ReadRequiredInt(*Item, TEXT("modelId"), Placed.ModelId, OutError) ||
			!ReadRequiredInt(*Item, TEXT("x"), Placed.X, OutError) ||
			!ReadRequiredInt(*Item, TEXT("y"), Placed.Y, OutError))
		{
			return false;
		}

		// Остальные поля необязательны: у части объектов они нулевые и могут
		// отсутствовать в будущих версиях манифеста.
		(*Item)->TryGetNumberField(TEXT("type"), Placed.Type);
		(*Item)->TryGetNumberField(TEXT("heightOff"), Placed.HeightOff);
		(*Item)->TryGetNumberField(TEXT("yaw"), Placed.Yaw);
		(*Item)->TryGetNumberField(TEXT("scale"), Placed.Scale);

		OutManifest.Objects.Add(Placed);
	}

	UE_LOG(LogCorsairsManifest, Log,
		   TEXT("Манифест %s: секций %dx%d, объектов %d"),
		   *FPaths::GetCleanFilename(FilePath),
		   OutManifest.SectionCntX, OutManifest.SectionCntY,
		   OutManifest.Objects.Num());

	return true;
}

FVector UCorsairsSceneManifestLibrary::GetObjectLocation(const FCorsairsPlacedObject& Object,
														 float UnitsPerTile)
{
	// 100 исходных единиц = один тайл. Y инвертируется: без этого карта
	// оказывается зеркальной относительно оригинала.
	const float Scale = UnitsPerTile / 100.0f;
	return FVector(static_cast<float>(Object.X) * Scale,
				   -static_cast<float>(Object.Y) * Scale,
				   static_cast<float>(Object.HeightOff) * Scale);
}

FRotator UCorsairsSceneManifestLibrary::GetObjectRotation(const FCorsairsPlacedObject& Object)
{
	// Yaw в файле — десятые доли градуса.
	return FRotator(0.0f, static_cast<float>(Object.Yaw) / 10.0f, 0.0f);
}
