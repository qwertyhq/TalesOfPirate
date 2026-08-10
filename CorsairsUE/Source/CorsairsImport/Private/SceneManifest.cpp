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
	// 100 исходных единиц = один тайл; UnitsPerTile задаёт, сколько сантиметров
	// UE приходится на тайл.
	//
	// Базис — жёсткий поворот Q(x, y, z) = (-y, x, z), то есть поворот плоскости
	// XY на +90 градусов. Это именно поворот, а не отражение: определитель Q
	// равен +1. Разница принципиальная — у отражения определитель -1, оно
	// меняет ориентацию плоскости, а вместе с ней обход треугольников, знак
	// угла и «право/лево» сцены, и каждый потребитель вынужден заводить
	// встречную компенсацию. Поворот ориентацию сохраняет, поэтому один и тот
	// же Q применяется ко всему миру без исключений: рельеф
	// (tools/AssetConverter/src/TerrainPageMeshWriter.cpp:507-510 — вершины,
	// :719-721 — origin актора), постройки (здесь), персонаж
	// (FCorsairsCharacterGround::ActorCenter) и камера. Эталон —
	// CorsairsUE/Scripts/scene_coordinate_basis.py, source_location_to_ue.
	const float Scale = UnitsPerTile / 100.0f;
	return FVector(-static_cast<float>(Object.Y) * Scale,
				   static_cast<float>(Object.X) * Scale,
				   static_cast<float>(Object.HeightOff) * Scale);
}

FRotator UCorsairsSceneManifestLibrary::GetObjectRotation(const FCorsairsPlacedObject& Object)
{
	// Yaw в манифесте — ЦЕЛЫЕ градусы, а не десятые доли: Angle2Radian(a)
	// считает a*PI/180 (sources/Engine/Util/MPMath.h), редактор карт крутит
	// объект шагом 5 и привязывает к сетке как nYaw/45*45
	// (sources/Client/src/Tools/Editor/MPEditor.cpp:988-1008), а по данным
	// garner все углы кратны пяти и по модулю 360 дают ровно 72 значения.
	// Делить на десять нельзя — это сплющивало сцену в диапазон 36 градусов.
	//
	// Сдвиг -90 градусов: собственный угол легаси-объекта равен θ-180
	// (sources/Engine/Model/lwObjectMethod.cpp:41 плюс row-vector раскладка
	// sources/Engine/Math/lwMath.inl:1484-1489), а поворот Q добавляет +90.
	// В сумме yaw_ue = source_yaw - 90 — то же, что делает
	// source_scene_yaw_to_ue в Scripts/scene_coordinate_basis.py.
	// NormalizeAxis сворачивает результат в (-180, 180], как unwind_degrees там же.
	return FRotator(0.0f,
					FRotator::NormalizeAxis(static_cast<float>(Object.Yaw) - 90.0f),
					0.0f);
}
