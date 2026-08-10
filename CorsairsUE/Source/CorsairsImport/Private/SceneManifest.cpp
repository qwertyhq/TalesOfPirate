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

FVector UCorsairsSceneManifestLibrary::MapPointToWorld(int32 MapX, int32 MapY,
													   float HeightCm, float UnitsPerTile)
{
	// 100 исходных единиц = один тайл. Оси переносятся один в один: карта
	// MindPower3D и мир Unreal обе левосторонние с высотой по Z, и ничего
	// переставлять не нужно.
	//
	// Это не самоочевидно, потому что модель по пути дважды меняет оси —
	// и обе замены гасят друг друга. Конвертер меняет местами Y и Z при
	// записи glTF (там «вверх» — это Y), а Interchange при импорте делает
	// ровно ту же перестановку обратно. Измерено: страница рельефа с
	// координатами glTF X 1024..1536, Y -3.7..2.4, Z 1536..2048 приходит в
	// редактор как X 102400..153600, Y 153600..204800, Z -370..240.
	//
	// Прежний вариант отрицал Y. Земля тогда отрицала ту же ось, поэтому
	// город и рельеф сходились между собой — но обе половины оказывались
	// зеркальными относительно оригинала. Отражение меняет рукость мира,
	// и заметить его можно лишь сверкой с оригиналом по взаимному положению
	// кварталов: сама по себе зеркальная карта выглядит совершенно обычно.
	const float Scale = UnitsPerTile / 100.0f;
	return FVector(static_cast<float>(MapX) * Scale,
				   static_cast<float>(MapY) * Scale,
				   HeightCm * Scale);
}

FIntPoint UCorsairsSceneManifestLibrary::WorldPointToMap(const FVector& WorldLocation,
														 float UnitsPerTile)
{
	// Обратная к MapPointToWorld: то же соответствие осей, обратный масштаб.
	const float Scale = UnitsPerTile / 100.0f;
	return FIntPoint(FMath::RoundToInt(WorldLocation.X / Scale),
					 FMath::RoundToInt(WorldLocation.Y / Scale));
}

FVector UCorsairsSceneManifestLibrary::GetObjectLocation(const FCorsairsPlacedObject& Object,
														 float UnitsPerTile)
{
	return MapPointToWorld(Object.X, Object.Y,
						   static_cast<float>(Object.HeightOff), UnitsPerTile);
}

FRotator UCorsairsSceneManifestLibrary::MapYawToWorld(int32 Yaw)
{
	// Yaw везде — целые градусы. Оригинал переводит его в радианы напрямую
	// (`Angle2Radian(_nYaw)` в SceneObj.h, а сама формула — умножение на π/180
	// в MPMath.h), никакого делителя там нет. Редактор карт подтверждает это
	// независимо: поворот выделенного объекта идёт шагом в пять единиц, а
	// привязка к сетке считается как `nYaw / 45 * 45` — для десятых долей
	// градуса сетка была бы 450.
	//
	// Угол переносится как есть — ни знак, ни величина не меняются: оси
	// плоскости карты совпадают с осями мира (см. GetObjectLocation).
	//
	// Поправка вынесена отдельной величиной, чтобы её было где задать, если
	// модели начнут приезжать с другим направлением «вперёд». Искать такую
	// правку по колсайтам — верный способ получить наполовину повёрнутый мир.
	constexpr float YawCorrection = 0.0f;
	return FRotator(0.0f, static_cast<float>(Yaw) + YawCorrection, 0.0f);
}

FRotator UCorsairsSceneManifestLibrary::GetObjectRotation(const FCorsairsPlacedObject& Object)
{
	return MapYawToWorld(Object.Yaw);
}
