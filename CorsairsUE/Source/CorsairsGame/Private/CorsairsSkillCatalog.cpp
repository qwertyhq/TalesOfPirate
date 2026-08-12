#include "CorsairsSkillCatalog.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr int64 MaxSkillId = static_cast<int64>(MAX_uint32);
const TSet<FString> RootFields = {TEXT("schemaVersion"), TEXT("skills")};
const TSet<FString> SkillFields = {
	TEXT("skillId"), TEXT("name"), TEXT("applyDistance"),
	TEXT("applyTarget"), TEXT("applyType"), TEXT("helpful"),
	TEXT("habitatMask"), TEXT("radius"), TEXT("shape"), TEXT("targetMode")};

bool HasOnlyFields(
	const TSharedPtr<FJsonObject>& Object,
	const TSet<FString>& Allowed,
	const FString& Path,
	FString& OutError)
{
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			OutError = FString::Printf(TEXT("%s.%s: unknown field"), *Path, *Pair.Key);
			return false;
		}
	}
	return true;
}

bool ReadInteger(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	int64 Minimum,
	int64 Maximum,
	int64& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (Value == nullptr || !Value->IsValid() || (*Value)->Type != EJson::Number)
	{
		OutError = FString::Printf(TEXT("%s.%s: expected integer"), *Path, Field);
		return false;
	}
	const double Number = (*Value)->AsNumber();
	if (!FMath::IsFinite(Number) || FMath::FloorToDouble(Number) != Number ||
		Number < static_cast<double>(Minimum) || Number > static_cast<double>(Maximum))
	{
		OutError = FString::Printf(TEXT("%s.%s: integer is out of range"), *Path, Field);
		return false;
	}
	OutValue = static_cast<int64>(Number);
	return true;
}

bool ReadString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	FString& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (Value == nullptr || !Value->IsValid() || (*Value)->Type != EJson::String ||
		!(*Value)->TryGetString(OutValue) || OutValue.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s.%s: expected string"), *Path, Field);
		return false;
	}
	return true;
}

bool ReadBoolean(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	bool& OutValue,
	FString& OutError)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (Value == nullptr || !Value->IsValid() || (*Value)->Type != EJson::Boolean)
	{
		OutError = FString::Printf(TEXT("%s.%s: expected boolean"), *Path, Field);
		return false;
	}
	OutValue = (*Value)->AsBool();
	return true;
}

bool ReadTargetMode(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Path,
	ECorsairsSkillTargetMode& OutMode,
	FString& OutError)
{
	FString Value;
	if (!ReadString(Object, TEXT("targetMode"), Path, Value, OutError))
	{
		return false;
	}
	if (Value == TEXT("self"))
	{
		OutMode = ECorsairsSkillTargetMode::Self;
		return true;
	}
	if (Value == TEXT("entity"))
	{
		OutMode = ECorsairsSkillTargetMode::Entity;
		return true;
	}
	if (Value == TEXT("ground"))
	{
		OutMode = ECorsairsSkillTargetMode::Ground;
		return true;
	}
	if (Value == TEXT("unsupported"))
	{
		OutMode = ECorsairsSkillTargetMode::Unsupported;
		return true;
	}
	OutError = FString::Printf(TEXT("%s.targetMode: unsupported value"), *Path);
	return false;
}

ECorsairsSkillTargetMode TargetModeForApplyType(int32 ApplyType)
{
	if (ApplyType == 1 || ApplyType == 3)
	{
		return ECorsairsSkillTargetMode::Entity;
	}
	if (ApplyType == 2)
	{
		return ECorsairsSkillTargetMode::Ground;
	}
	return ECorsairsSkillTargetMode::Unsupported;
}
}

bool FCorsairsSkillCatalog::Load(const FString& JsonPath, FString& OutError)
{
	OutError.Empty();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *JsonPath))
	{
		OutError = FString::Printf(TEXT("%s: could not read file"), *JsonPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s: invalid JSON"), *JsonPath);
		return false;
	}
	if (!HasOnlyFields(Root, RootFields, TEXT("root"), OutError))
	{
		return false;
	}

	int64 SchemaVersion = 0;
	if (!ReadInteger(Root, TEXT("schemaVersion"), TEXT("root"), 1, 1, SchemaVersion, OutError))
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* SkillsValue = Root->Values.Find(TEXT("skills"));
	if (SkillsValue == nullptr || !SkillsValue->IsValid() || (*SkillsValue)->Type != EJson::Array)
	{
		OutError = TEXT("root.skills: expected array");
		return false;
	}

	TMap<int64, FCorsairsSkillDefinition> LoadedSkills;
	for (int32 Index = 0; Index < (*SkillsValue)->AsArray().Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*SkillsValue)->AsArray()[Index];
		const FString Path = FString::Printf(TEXT("skills[%d]"), Index);
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			OutError = FString::Printf(TEXT("%s: expected object"), *Path);
			return false;
		}
		if (!HasOnlyFields(*Object, SkillFields, Path, OutError))
		{
			return false;
		}

		FCorsairsSkillDefinition Definition;
		int64 Number = 0;
		if (!ReadInteger(*Object, TEXT("skillId"), Path, 1, MaxSkillId, Number, OutError))
		{
			return false;
		}
		Definition.SkillId = Number;
		if (LoadedSkills.Contains(Definition.SkillId))
		{
			OutError = FString::Printf(TEXT("%s.skillId: duplicate value"), *Path);
			return false;
		}
		if (!ReadString(*Object, TEXT("name"), Path, Definition.Name, OutError) ||
			!ReadInteger(*Object, TEXT("applyDistance"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.ApplyDistance = static_cast<int32>(Number);
		if (!ReadInteger(*Object, TEXT("applyTarget"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.ApplyTarget = static_cast<int32>(Number);
		if (!ReadInteger(*Object, TEXT("applyType"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.ApplyType = static_cast<int32>(Number);
		if (!ReadBoolean(*Object, TEXT("helpful"), Path, Definition.bHelpful, OutError) ||
			!ReadInteger(*Object, TEXT("habitatMask"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.HabitatMask = static_cast<int32>(Number);
		if (!ReadInteger(*Object, TEXT("radius"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.Radius = static_cast<int32>(Number);
		if (!ReadInteger(*Object, TEXT("shape"), Path, 0, MAX_int32, Number, OutError))
		{
			return false;
		}
		Definition.Shape = static_cast<int32>(Number);
		if (!ReadTargetMode(*Object, Path, Definition.TargetMode, OutError))
		{
			return false;
		}
		if (Definition.TargetMode != TargetModeForApplyType(Definition.ApplyType))
		{
			OutError = FString::Printf(TEXT("%s.targetMode: does not match applyType"), *Path);
			return false;
		}
		LoadedSkills.Add(Definition.SkillId, MoveTemp(Definition));
	}

	Skills = MoveTemp(LoadedSkills);
	return true;
}

const FCorsairsSkillDefinition* FCorsairsSkillCatalog::Find(int64 SkillId) const
{
	return Skills.Find(SkillId);
}

int32 FCorsairsSkillCatalog::Num() const
{
	return Skills.Num();
}
