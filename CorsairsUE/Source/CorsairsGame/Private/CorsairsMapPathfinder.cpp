#include "CorsairsMapPathfinder.h"

#include "Algo/Reverse.h"

namespace
{
const FIntPoint NeighbourOffsets[] = {
	FIntPoint(0, -1),
	FIntPoint(0, 1),
	FIntPoint(-1, 0),
	FIntPoint(1, 0),
	FIntPoint(1, -1),
	FIntPoint(1, 1),
	FIntPoint(-1, 1),
	FIntPoint(-1, -1),
};

int32 PointToCell(const int32 Value)
{
	return FMath::FloorToInt(
		static_cast<double>(Value) /
		static_cast<double>(FCorsairsMapPathfinder::CellSize));
}

FIntPoint QuantizeCell(const FIntPoint Point)
{
	return FIntPoint(PointToCell(Point.X), PointToCell(Point.Y));
}

FIntPoint CellCenter(const FIntPoint Cell)
{
	return FIntPoint(
		Cell.X * FCorsairsMapPathfinder::CellSize +
		FCorsairsMapPathfinder::CellCenterOffset,
		Cell.Y * FCorsairsMapPathfinder::CellSize +
		FCorsairsMapPathfinder::CellCenterOffset);
}

bool IsInsideBounds(
	const FIntRect Bounds,
	const FIntPoint Cell)
{
	const FIntPoint Center = CellCenter(Cell);
	return Center.X >= Bounds.Min.X && Center.Y >= Bounds.Min.Y &&
		Center.X < Bounds.Max.X && Center.Y < Bounds.Max.Y;
}

bool IsOpen(
	const FCorsairsCharacterGround& Ground,
	const FIntPoint Cell,
	const ECorsairsTraversalKind Traversal,
	const FIntRect Bounds)
{
	if (!IsInsideBounds(Bounds, Cell))
	{
		return false;
	}
	FCorsairsNavigationCell NavigationCell;
	return Ground.TrySampleNavigation(
		CellCenter(Cell), Traversal, NavigationCell) &&
		!NavigationCell.bBlocked;
}

void AddUnique(TArray<FIntPoint>& Cells, const FIntPoint Cell)
{
	if (Cells.IsEmpty() || Cells.Last() != Cell)
	{
		Cells.Add(Cell);
	}
}

TArray<FIntPoint> BuildSupercover(
	const FIntPoint Start,
	const FIntPoint Target)
{
	TArray<FIntPoint> Cells;
	Cells.Reserve(
		FMath::Abs(Target.X - Start.X) +
		FMath::Abs(Target.Y - Start.Y) + 2);
	AddUnique(Cells, Start);

	const int32 DeltaX = Target.X - Start.X;
	const int32 DeltaY = Target.Y - Start.Y;
	const int32 StepX = DeltaX < 0 ? -1 : 1;
	const int32 StepY = DeltaY < 0 ? -1 : 1;
	const int32 CountX = FMath::Abs(DeltaX);
	const int32 CountY = FMath::Abs(DeltaY);
	int32 X = Start.X;
	int32 Y = Start.Y;
	int32 IndexX = 0;
	int32 IndexY = 0;
	while (X != Target.X || Y != Target.Y)
	{
		const int64 Left =
			static_cast<int64>(1 + 2 * IndexX) * CountY;
		const int64 Right =
			static_cast<int64>(1 + 2 * IndexY) * CountX;
		if (Left == Right)
		{
			const int32 PreviousX = X;
			const int32 PreviousY = Y;
			X += StepX;
			++IndexX;
			AddUnique(Cells, FIntPoint(X, PreviousY));
			Y += StepY;
			++IndexY;
			AddUnique(Cells, FIntPoint(PreviousX, Y));
			AddUnique(Cells, FIntPoint(X, Y));
		}
		else if (Left < Right)
		{
			X += StepX;
			++IndexX;
			AddUnique(Cells, FIntPoint(X, Y));
		}
		else
		{
			Y += StepY;
			++IndexY;
			AddUnique(Cells, FIntPoint(X, Y));
		}
	}
	return Cells;
}

bool IsDiagonalTransitionSafe(
	const FIntPoint Previous,
	const FIntPoint Current,
	const FCorsairsCharacterGround& Ground,
	const ECorsairsTraversalKind Traversal,
	const FIntRect Bounds)
{
	const int32 DeltaX = Current.X - Previous.X;
	const int32 DeltaY = Current.Y - Previous.Y;
	if (FMath::Abs(DeltaX) != 1 || FMath::Abs(DeltaY) != 1)
	{
		return true;
	}
	return IsOpen(
		Ground,
		FIntPoint(Current.X, Previous.Y),
		Traversal,
		Bounds) &&
		IsOpen(
			Ground,
		FIntPoint(Previous.X, Current.Y),
			Traversal,
			Bounds);
}

bool IsVisible(
	const FCorsairsCharacterGround& Ground,
	const FIntPoint Start,
	const FIntPoint Target,
	const ECorsairsTraversalKind Traversal,
	const FIntRect Bounds)
{
	const TArray<FIntPoint> Cover = BuildSupercover(Start, Target);
	for (int32 Index = 0; Index < Cover.Num(); ++Index)
	{
		if (!IsOpen(Ground, Cover[Index], Traversal, Bounds))
		{
			return false;
		}
		if (Index > 0 && !IsDiagonalTransitionSafe(
				Cover[Index - 1], Cover[Index], Ground, Traversal, Bounds))
		{
			return false;
		}
	}
	return true;
}

TArray<FIntPoint> SafeStraightPrefix(
	const FCorsairsCharacterGround& Ground,
	const FIntPoint Start,
	const FIntPoint Target,
	const ECorsairsTraversalKind Traversal,
	const FIntRect Bounds)
{
	TArray<FIntPoint> Prefix;
	const TArray<FIntPoint> Cover = BuildSupercover(Start, Target);
	for (int32 Index = 0; Index < Cover.Num(); ++Index)
	{
		if (!IsOpen(Ground, Cover[Index], Traversal, Bounds) ||
			(Index > 0 && !IsDiagonalTransitionSafe(
				Cover[Index - 1], Cover[Index], Ground, Traversal, Bounds)))
		{
			break;
		}
		Prefix.Add(Cover[Index]);
	}
	if (Prefix.IsEmpty() && IsOpen(Ground, Start, Traversal, Bounds))
	{
		Prefix.Add(Start);
	}
	return Prefix;
}

TArray<FIntPoint> CompressDirections(const TArray<FIntPoint>& Cells)
{
	TArray<FIntPoint> Waypoints;
	if (Cells.IsEmpty())
	{
		return Waypoints;
	}
	Waypoints.Add(CellCenter(Cells[0]));
	if (Cells.Num() == 1)
	{
		return Waypoints;
	}

	int32 LastDirectionX = FMath::Sign(Cells[1].X - Cells[0].X);
	int32 LastDirectionY = FMath::Sign(Cells[1].Y - Cells[0].Y);
	for (int32 Index = 2; Index < Cells.Num(); ++Index)
	{
		const int32 DirectionX = FMath::Sign(
			Cells[Index].X - Cells[Index - 1].X);
		const int32 DirectionY = FMath::Sign(
			Cells[Index].Y - Cells[Index - 1].Y);
		if (DirectionX != LastDirectionX || DirectionY != LastDirectionY)
		{
			Waypoints.Add(CellCenter(Cells[Index - 1]));
			LastDirectionX = DirectionX;
			LastDirectionY = DirectionY;
		}
	}
	Waypoints.Add(CellCenter(Cells.Last()));
	return Waypoints;
}

bool FindRetreatTarget(
	const FCorsairsCharacterGround& Ground,
	const FIntPoint Start,
	const FIntPoint Target,
	const ECorsairsTraversalKind Traversal,
	const FIntRect Bounds,
	FIntPoint& OutTarget)
{
	if (IsOpen(Ground, Target, Traversal, Bounds))
	{
		OutTarget = Target;
		return true;
	}
	const int32 DeltaX = Start.X - Target.X;
	const int32 DeltaY = Start.Y - Target.Y;
	const int32 Steps = FMath::Max(FMath::Abs(DeltaX), FMath::Abs(DeltaY));
	for (int32 Step = 1; Step <= Steps; ++Step)
	{
		const FIntPoint Candidate(
			Target.X + FMath::RoundToInt(
				static_cast<double>(DeltaX) * Step / Steps),
			Target.Y + FMath::RoundToInt(
				static_cast<double>(DeltaY) * Step / Steps));
		if (IsOpen(Ground, Candidate, Traversal, Bounds))
		{
			OutTarget = Candidate;
			return true;
		}
	}
	return false;
}

TArray<FIntPoint> Reconstruct(
	const TMap<FIntPoint, FIntPoint>& Parents,
	const FIntPoint Start,
	const FIntPoint Target)
{
	TArray<FIntPoint> Cells;
	FIntPoint Current = Target;
	Cells.Add(Current);
	while (Current != Start)
	{
		const FIntPoint* Parent = Parents.Find(Current);
		if (Parent == nullptr)
		{
			Cells.Reset();
			return Cells;
		}
		Current = *Parent;
		Cells.Add(Current);
	}
	Algo::Reverse(Cells);
	return Cells;
}

void ApplyWireLimit(FCorsairsPathResult& Result)
{
	if (Result.Waypoints.Num() <= FCorsairsMapPathfinder::MaxWireWaypoints)
	{
		return;
	}
	Result.Waypoints.SetNum(FCorsairsMapPathfinder::MaxWireWaypoints);
	Result.bTruncated = true;
	Result.Status = ECorsairsPathStatus::Partial;
}
}

FCorsairsPathResult FCorsairsMapPathfinder::FindPath(
	const FCorsairsCharacterGround& Ground,
	const FIntPoint Start,
	const FIntPoint Target,
	const ECorsairsPathMode Mode,
	const ECorsairsTraversalKind Traversal)
{
	FCorsairsPathResult Result;
	Result.RequestedTarget = Target;
	if (!Ground.IsNavigationLoaded())
	{
		return Result;
	}
	const FIntRect Bounds = Ground.GetSourceBounds();
	const FIntPoint StartCell = QuantizeCell(Start);
	const FIntPoint RequestedCell = QuantizeCell(Target);
	if (!IsInsideBounds(Bounds, StartCell) ||
		!IsInsideBounds(Bounds, RequestedCell) ||
		!IsOpen(Ground, StartCell, Traversal, Bounds))
	{
		return Result;
	}

	FIntPoint ResolvedCell = FIntPoint::ZeroValue;
	if (!FindRetreatTarget(
			Ground,
			StartCell,
			RequestedCell,
			Traversal,
			Bounds,
			ResolvedCell))
	{
		return Result;
	}
	Result.ResolvedTarget = CellCenter(ResolvedCell);
	// Путь из одной точки не имеет смысла в протоколе: сервер ждёт 2–32 точки.
	// Клик внутри текущей half-cell — честное отсутствие действия, а не две
	// продублированные координаты.
	if (StartCell == ResolvedCell)
	{
		return Result;
	}

	if (IsVisible(
			Ground,
			StartCell,
			ResolvedCell,
			Traversal,
			Bounds))
	{
		Result.Status = ECorsairsPathStatus::Found;
		Result.Waypoints.Add(CellCenter(StartCell));
		Result.Waypoints.Add(CellCenter(ResolvedCell));
		ApplyWireLimit(Result);
		return Result;
	}

	if (Mode == ECorsairsPathMode::StraightOnly)
	{
		Result.Status = ECorsairsPathStatus::Partial;
		Result.Waypoints = CompressDirections(SafeStraightPrefix(
			Ground,
			StartCell,
			ResolvedCell,
			Traversal,
			Bounds));
		ApplyWireLimit(Result);
		return Result;
	}

	TArray<FIntPoint> Queue;
	Queue.Reserve(FCorsairsMapPathfinder::MaxVisitedCells);
	Queue.Add(StartCell);
	int32 QueueHead = 0;
	TSet<FIntPoint> Visited;
	Visited.Reserve(FCorsairsMapPathfinder::MaxVisitedCells);
	Visited.Add(StartCell);
	TMap<FIntPoint, FIntPoint> Parents;
	Parents.Reserve(FCorsairsMapPathfinder::MaxVisitedCells);
	bool bFound = StartCell == ResolvedCell;
	bool bBudgetExhausted = false;
	while (!bFound && QueueHead < Queue.Num())
	{
		const FIntPoint Current = Queue[QueueHead++];
		for (const FIntPoint Offset : NeighbourOffsets)
		{
			const FIntPoint Candidate = Current + Offset;
			if (Visited.Contains(Candidate))
			{
				continue;
			}
			if (Visited.Num() >= FCorsairsMapPathfinder::MaxVisitedCells)
			{
				bBudgetExhausted = true;
				break;
			}
			if (!IsOpen(Ground, Candidate, Traversal, Bounds))
			{
				continue;
			}
			Visited.Add(Candidate);
			Parents.Add(Candidate, Current);
			Queue.Add(Candidate);
			if (Candidate == ResolvedCell)
			{
				bFound = true;
				break;
			}
		}
		if (bBudgetExhausted)
		{
			break;
		}
	}

	if (bFound)
	{
		Result.Status = ECorsairsPathStatus::Found;
		Result.Waypoints = CompressDirections(Reconstruct(
			Parents, StartCell, ResolvedCell));
		ApplyWireLimit(Result);
		return Result;
	}

	Result.Status = ECorsairsPathStatus::Partial;
	Result.Waypoints = CompressDirections(SafeStraightPrefix(
		Ground,
		StartCell,
		ResolvedCell,
		Traversal,
		Bounds));
	ApplyWireLimit(Result);
	return Result;
}
