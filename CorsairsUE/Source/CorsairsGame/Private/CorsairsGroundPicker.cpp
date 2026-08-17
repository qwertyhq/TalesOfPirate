#include "CorsairsGroundPicker.h"

namespace
{
constexpr double MaxRayParameter = 1000000.0;
constexpr double MarchStep = 25.0;
constexpr double Epsilon = 1.0e-9;

bool ClipAxis(
	const double Origin,
	const double Direction,
	const double Minimum,
	const double Maximum,
	double& InOutEnter,
	double& InOutExit)
{
	if (FMath::Abs(Direction) <= Epsilon)
	{
		return Origin >= Minimum && Origin <= Maximum;
	}

	const double Reciprocal = 1.0 / Direction;
	double Near = (Minimum - Origin) * Reciprocal;
	double Far = (Maximum - Origin) * Reciprocal;
	if (Near > Far)
	{
		const double Temporary = Near;
		Near = Far;
		Far = Temporary;
	}
	InOutEnter = FMath::Max(InOutEnter, Near);
	InOutExit = FMath::Min(InOutExit, Far);
	return InOutEnter <= InOutExit;
}

bool SampleDifference(
	const FCorsairsCharacterGround& Ground,
	const FVector2d SourceOrigin,
	const FVector2d SourceDirection,
	const double OriginZ,
	const double DirectionZ,
	const double RayParameter,
	double& OutDifference,
	FVector2d& OutPoint,
	double& OutHeight)
{
	OutPoint = SourceOrigin + SourceDirection * RayParameter;
	if (!Ground.TrySampleSurface(OutPoint, OutHeight))
	{
		return false;
	}
	const double RayZ = OriginZ + DirectionZ * RayParameter;
	OutDifference = RayZ - OutHeight;
	return FMath::IsFinite(OutDifference);
}

bool ResolveBracket(
	const FCorsairsCharacterGround& Ground,
	const FVector2d SourceOrigin,
	const FVector2d SourceDirection,
	const double OriginZ,
	const double DirectionZ,
	double LowParameter,
	double HighParameter,
	double LowDifference,
	double HighDifference,
	FVector2d& OutPoint,
	double& OutHeight)
{
	for (int32 Iteration = 0; Iteration < 32; ++Iteration)
	{
		const double MidParameter =
			LowParameter + (HighParameter - LowParameter) * 0.5;
		double MidDifference = 0.0;
		FVector2d MidPoint = FVector2d::ZeroVector;
		double MidHeight = 0.0;
		if (!SampleDifference(
				Ground,
				SourceOrigin,
				SourceDirection,
				OriginZ,
				DirectionZ,
				MidParameter,
				MidDifference,
				MidPoint,
				MidHeight))
		{
			return false;
		}
		if (FMath::Abs(MidDifference) <= Epsilon)
		{
			OutPoint = MidPoint;
			OutHeight = MidHeight;
			return true;
		}
		if ((LowDifference < 0.0 && MidDifference > 0.0) ||
			(LowDifference > 0.0 && MidDifference < 0.0))
		{
			HighParameter = MidParameter;
			HighDifference = MidDifference;
		}
		else
		{
			LowParameter = MidParameter;
			LowDifference = MidDifference;
		}
	}

	const double FinalParameter =
		LowParameter + (HighParameter - LowParameter) * 0.5;
	double FinalDifference = 0.0;
	return SampleDifference(
		Ground,
		SourceOrigin,
		SourceDirection,
		OriginZ,
		DirectionZ,
		FinalParameter,
		FinalDifference,
		OutPoint,
		OutHeight);
}
}

bool FCorsairsGroundPicker::TryPick(
	const FVector& RayOrigin,
	const FVector& RayDirection,
	const FCorsairsCharacterGround& Ground,
	FVector2d& OutSourcePoint,
	double& OutHeightCm)
{
	OutSourcePoint = FVector2d::ZeroVector;
	OutHeightCm = 0.0;
	if (!Ground.IsNavigationLoaded() ||
		!FMath::IsFinite(RayOrigin.X) || !FMath::IsFinite(RayOrigin.Y) ||
		!FMath::IsFinite(RayOrigin.Z) ||
		!FMath::IsFinite(RayDirection.X) ||
		!FMath::IsFinite(RayDirection.Y) ||
		!FMath::IsFinite(RayDirection.Z) ||
		RayDirection.SizeSquared() <= SMALL_NUMBER)
	{
		return false;
	}

	const FIntRect Bounds = Ground.GetSourceBounds();
	const double MaximumX = static_cast<double>(Bounds.Max.X - 100) - Epsilon;
	const double MaximumY = static_cast<double>(Bounds.Max.Y - 100) - Epsilon;
	if (MaximumX <= static_cast<double>(Bounds.Min.X) ||
		MaximumY <= static_cast<double>(Bounds.Min.Y))
	{
		return false;
	}

	const FVector2d SourceOrigin(RayOrigin.Y, -RayOrigin.X);
	const FVector2d SourceDirection(RayDirection.Y, -RayDirection.X);
	double Enter = 0.0;
	double Exit = MaxRayParameter;
	if (!ClipAxis(
			SourceOrigin.X,
			SourceDirection.X,
			static_cast<double>(Bounds.Min.X),
			MaximumX,
			Enter,
			Exit) ||
		!ClipAxis(
			SourceOrigin.Y,
			SourceDirection.Y,
			static_cast<double>(Bounds.Min.Y),
			MaximumY,
			Enter,
			Exit))
	{
		return false;
	}
	if (Exit < 0.0)
	{
		return false;
	}
	Enter = FMath::Max(Enter, 0.0);
	if (Enter > Exit)
	{
		return false;
	}

	const double HorizontalSpeed = SourceDirection.Size();
	const double RaySpeed = FMath::Max(HorizontalSpeed, FMath::Abs(RayDirection.Z));
	const double ParameterStep =
		RaySpeed > Epsilon ? MarchStep / RaySpeed : MarchStep;
	double PreviousParameter = Enter;
	double PreviousDifference = 0.0;
	FVector2d PreviousPoint = FVector2d::ZeroVector;
	double PreviousHeight = 0.0;
	if (!SampleDifference(
			Ground,
			SourceOrigin,
			SourceDirection,
			RayOrigin.Z,
			RayDirection.Z,
			PreviousParameter,
			PreviousDifference,
			PreviousPoint,
			PreviousHeight))
	{
		const double FirstValidParameter = Enter + ParameterStep;
		if (FirstValidParameter > Exit ||
			!SampleDifference(
				Ground,
				SourceOrigin,
				SourceDirection,
				RayOrigin.Z,
				RayDirection.Z,
				FirstValidParameter,
				PreviousDifference,
				PreviousPoint,
				PreviousHeight))
		{
			return false;
		}
		PreviousParameter = FirstValidParameter;
	}
	if (FMath::Abs(PreviousDifference) <= Epsilon)
	{
		OutSourcePoint = PreviousPoint;
		OutHeightCm = PreviousHeight;
		return true;
	}

	for (double Parameter = PreviousParameter + ParameterStep;
		Parameter <= Exit + Epsilon;
		Parameter += ParameterStep)
	{
		const double CurrentParameter = FMath::Min(Parameter, Exit);
		double CurrentDifference = 0.0;
		FVector2d CurrentPoint = FVector2d::ZeroVector;
		double CurrentHeight = 0.0;
		if (!SampleDifference(
				Ground,
				SourceOrigin,
				SourceDirection,
				RayOrigin.Z,
				RayDirection.Z,
				CurrentParameter,
				CurrentDifference,
				CurrentPoint,
				CurrentHeight))
		{
			continue;
		}
		if (FMath::Abs(CurrentDifference) <= Epsilon ||
			(PreviousDifference < 0.0 && CurrentDifference > 0.0) ||
			(PreviousDifference > 0.0 && CurrentDifference < 0.0))
		{
			if (FMath::Abs(CurrentDifference) <= Epsilon)
			{
				OutSourcePoint = CurrentPoint;
				OutHeightCm = CurrentHeight;
				return true;
			}
			return ResolveBracket(
				Ground,
				SourceOrigin,
				SourceDirection,
				RayOrigin.Z,
				RayDirection.Z,
				PreviousParameter,
				CurrentParameter,
				PreviousDifference,
				CurrentDifference,
				OutSourcePoint,
				OutHeightCm);
		}
		PreviousParameter = CurrentParameter;
		PreviousDifference = CurrentDifference;
		PreviousPoint = CurrentPoint;
		PreviousHeight = CurrentHeight;
		if (CurrentParameter >= Exit)
		{
			break;
		}
	}
	return false;
}

bool FCorsairsGroundPicker::TryPick(
	const FVector& RayOrigin,
	const FVector& RayDirection,
	const FCorsairsCharacterGround& Ground,
	FIntPoint& OutSourcePoint)
{
	FVector2d SourcePoint = FVector2d::ZeroVector;
	double HeightCm = 0.0;
	if (!TryPick(
			RayOrigin,
			RayDirection,
			Ground,
			SourcePoint,
			HeightCm))
	{
		return false;
	}
	OutSourcePoint = FIntPoint(
		FMath::RoundToInt(SourcePoint.X),
		FMath::RoundToInt(SourcePoint.Y));
	return true;
}
