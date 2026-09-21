// Fill out your copyright notice in the Description page of Project Settings.

#include "SwarmManager.h"
#include "RatCPPBase.h"
#include "GameFramework/Character.h"
#include "Components/BoxComponent.h"
#include "Math/UnrealMathUtility.h"
#include "Kismet/GameplayStatics.h"
#include "Async/ParallelFor.h"
#include "Kismet/KismetMathLibrary.h"

// Sets default values
ASwarmManager::ASwarmManager()
{
	PrimaryActorTick.bCanEverTick = true;

	//Creates the default bounding box 
	BoundingBox = CreateDefaultSubobject<UBoxComponent>(TEXT("MyCollisionBox"));
	RootComponent = BoundingBox;

	//Creates the Instanced Static Mesh
	EnemyInstancedMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InstancedMeshComp"));

	//Shadows are expensive
	EnemyInstancedMesh->SetCastShadow(false);
	EnemyInstancedMesh->bCastDynamicShadow = false;

	//We are using avoidance-based steering, we don't need collision
	EnemyInstancedMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	EnemyInstancedMesh->SetGenerateOverlapEvents(false);
}

//Helper function that does a line trace from the top of the Manager's volume to the bottom
FHitResult ASwarmManager::LineTraceWithinVolume(FVector2d CoordsInBounds, ECollisionChannel CollisionChannel, FCollisionQueryParams QueryParams)
{
	//Set some default information
	FVector BaseMeshBounds = EnemyInstancedMesh->GetStaticMesh()->GetBounds().BoxExtent;
	float ComponentHalfHeight = BoundingBox->Bounds.BoxExtent.Z;
	float ComponentWorldHeight = BoundingBox->Bounds.Origin.Z + BoundingBox->Bounds.BoxExtent.Z;


	//Line trace
	FHitResult TraceHitResult;
	FVector LineTraceStart = FVector{ CoordsInBounds, ComponentWorldHeight };
	FVector LineTraceEnd = LineTraceStart - FVector(0.0f, 0.0f, ComponentHalfHeight * 2.0f);

	GetWorld()->LineTraceSingleByChannel(
		TraceHitResult,
		LineTraceStart,
		LineTraceEnd,
		ECC_WorldStatic,
		QueryParams
	);

	return TraceHitResult;
}

//Helper function that converts world-space locations to the nearest index in the manager's flowmap
int ASwarmManager::GetFlowmapIndexAtWorldLocation(FVector Location)
{
	FVector RelativeLocation = (Location - FlowMapOrigin);
	float XIndex = FMath::FloorToInt(RelativeLocation.X / Resolution);
	float XOverflow = (RelativeLocation.X - XIndex);
	float YIndex = FMath::FloorToInt(RelativeLocation.Y / Resolution);
	float YOverflow = (RelativeLocation.Y - YIndex);

	float RoundedIndex = XIndex + (YIndex * ((width * 2) / Resolution));

	if (PlayerFlowmap.IsValidIndex(RoundedIndex))
	{
		return RoundedIndex;
	}

	return 0;
}

// Called when the game starts or when spawned
void ASwarmManager::BeginPlay()
{
	Super::BeginPlay();

	BoundingBox->OnComponentBeginOverlap.AddDynamic(this, &ASwarmManager::OnOverlapBegin);

	//Set a timer on which to update the flowmap
	GetWorldTimerManager().SetTimer(
		FlowMapUpdate,
		this,
		&ASwarmManager::UpdateFlowMaps,
		FlowMapTickRate,
		true
	);

	BoundingBox->UpdateBounds();

	//Pre-allocate the rat data arrays, 5000 is hardcoded, in the future this should be a variable
	RatData.SetNum(5000);
	RatRenderData.SetNum(5000);

	//Spawn all Instanced Static Meshes. Again, 5000 is hardcoded for now
	for (int32 i = 0; i < 5000; i++)
	{
		FVector BaseMeshBounds = EnemyInstancedMesh->GetStaticMesh()->GetBounds().BoxExtent;

		//Find a random location in bounds to spawn the mesh
		FVector2d AttemptedSpawnXYCoord = FVector2d{ FMath::RandPointInBox(BoundingBox->Bounds.GetBox()) };

		//Project it on to the actual height of the level
		FVector SpawnLocation = LineTraceWithinVolume(AttemptedSpawnXYCoord, ECC_WorldStatic, FCollisionQueryParams()).Location;
		
		//Spawn the mesh
		FTransform NewTransform;
		FVector Location = SpawnLocation + FVector{ 0, 0, BaseMeshBounds.Z };
		NewTransform.SetLocation(Location);

		int32 NewInstanceIndex = EnemyInstancedMesh->AddInstance(NewTransform);

		RatData[i] = FRatData{ Location, FVector::ZeroVector };
		RatRenderData[i] = NewTransform;
	}
}

// Called every frame
void ASwarmManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	MoveUnits(DeltaTime);
}

//Generates the Flowmap
void ASwarmManager::GenerateFlowMap(AActor* Target, TArray<FVector>* Array)
{
	//Calculate how many points we need
	width = BoundingBox->GetScaledBoxExtent().X;
	depth = BoundingBox->GetScaledBoxExtent().Y;
	
	int WidthInCells = FMath::FloorToInt((2 * width) / Resolution);
	int DepthInCells = FMath::FloorToInt((2 * depth) / Resolution);

	int NumberOfPoints = WidthInCells * DepthInCells;

	//Pre-allocate that many points in the array
	TArray<FVector> FlowMap;
	FlowMap.SetNum(NumberOfPoints);

	//Initialize them. FLT_MAX is used as a placeholder for a point that needs to be updated
	TArray<float> FlowMapWeights;
	FlowMapWeights.Init(FLT_MAX, NumberOfPoints);

	auto GetNeighborIndeces = [&](int Index) -> TArray<int32, TInlineAllocator<4>>
		{
			TArray<int32, TInlineAllocator<4>> Indeces;
			Indeces.Init(Index, 4);
			if ((Index - 1 >= 0) && ((Index) % WidthInCells != 0)) Indeces[0] = (Index - 1);
			if ((Index + 1 < NumberOfPoints) && ((Index + 1) % WidthInCells != 0)) Indeces[1] = (Index + 1);
			if (Index - WidthInCells > 0) Indeces[2] = (Index - WidthInCells);
			if (Index + WidthInCells < NumberOfPoints) Indeces[3] = (Index + WidthInCells);
			return Indeces;
		};

	//takes an index in the array and gets its coordinates in local space
	auto GetIndexRelativeCoordinates = [&](int Index) -> FVector2D
		{
			return FVector2D{ (Index % WidthInCells) * Resolution, (Index / WidthInCells) * Resolution };
		};

	//Generate flowmap weights
	if (FMath::IsWithinInclusive(Target->GetActorLocation().X, width * -1, width) && FMath::IsWithinInclusive(Target->GetActorLocation().Y, depth * -1, depth))
	{
		//Find the cell the target (player) is in
		FlowMapOrigin = { GetActorLocation().X - width, GetActorLocation().Y - depth, GetActorLocation().Z };

		int TargetCellXCoord;
		int TargetCellYCoord;
		int TargetCellIndex;
		FVector TargetLocationRelativeToFlowMap = Target->GetActorLocation() - FlowMapOrigin;

		//Get the coordinates of the cell the player is in
		TargetCellXCoord = TargetLocationRelativeToFlowMap.X / Resolution;
		TargetCellYCoord = TargetLocationRelativeToFlowMap.Y / Resolution;

		//Pre-allocate the heightmap array
		PlayerFlowmapFloorHeight.SetNum(NumberOfPoints);

		//Convert those coordinates into an array index
		TargetCellIndex = (TargetCellXCoord) + (TargetCellYCoord * WidthInCells);

		//Add flowmap weights to the queue

		TQueue<int32> FlowMapQueue;

		FlowMapWeights[TargetCellIndex] = 0;
		FlowMapQueue.Enqueue(TargetCellIndex);

		//Flowmap Generation Loop
		int CurrentIndex;
		while (FlowMapQueue.Dequeue(CurrentIndex))
		{
			for (int NeighborIndex : GetNeighborIndeces(CurrentIndex))
			{
				//Calculate the weight of this cell based on neighbor cells
				if (NeighborIndex != -1)
					if (FlowMapWeights[NeighborIndex] > FlowMapWeights[CurrentIndex] + 1)
					{
						FlowMapWeights[NeighborIndex] = FlowMapWeights[CurrentIndex] + 1;
						FlowMapQueue.Enqueue(NeighborIndex);
					}
			}
		}

		TArray<float> BlurOutputArray;
		BlurOutputArray.SetNum(NumberOfPoints);

		//Gaussian blur function, helps smooth out the flowmap so we don't have only orthogonal and diagonal directions
		auto GaussianBlur = [&](TArray<float>* FlowMap) -> void
			{
				TArray<float> OutputArray;
				OutputArray.SetNum(NumberOfPoints);

				for (int32 i = 0; i < FlowMapWeights.Num(); i++)
				{
					float sum = 0;
					int count = 0;
					for (int NeighborIndex : GetNeighborIndeces(i))
					{
						if (NeighborIndex == i)
						{
							sum = sum + FlowMapWeights[NeighborIndex] + 1;
							count++;
							continue;
						}
						sum = sum + FlowMapWeights[NeighborIndex];
						count++;
					}
					float average = sum / count;

					BlurOutputArray[i] = average;
				}

				*FlowMap = BlurOutputArray;
			};

		//Run the gaussian blur a few times. Tweak this to change flowmap smoothness, higher values impact performance
		for (int i = 0; i < 10; i++)
		{
			GaussianBlur(&FlowMapWeights);
		}
	
		//Generate flowmap vectors from flowmap weights
		for (int32 CurrentCellIndex = 0; CurrentCellIndex < FlowMapWeights.Num(); CurrentCellIndex++)
		{
			//Again, using MAX_int32 as a placeholder
			if (FlowMapWeights[CurrentCellIndex] == MAX_int32) continue;

			if (FlowMapWeights[CurrentCellIndex] == 0)
			{
				FlowMap[CurrentCellIndex] = FVector::ZeroVector; // No flow at the target
				continue;
			}

			FVector2D OutputVector;

			float OutputHorizontal;
			float OutputVertical;

			auto Neighbors = GetNeighborIndeces(CurrentCellIndex);

			//Get the X and Y components of our vector, with edge-case handling
			if (FlowMapWeights[Neighbors[0]] == FlowMapWeights[CurrentCellIndex] || FlowMapWeights[Neighbors[1]] == FlowMapWeights[CurrentCellIndex])
				OutputHorizontal = 2 * (FlowMapWeights[Neighbors[0]] - FlowMapWeights[Neighbors[1]]);
			else
				OutputHorizontal = (FlowMapWeights[Neighbors[0]] - FlowMapWeights[Neighbors[1]]);

			if (FlowMapWeights[Neighbors[2]] == FlowMapWeights[CurrentCellIndex] || FlowMapWeights[Neighbors[3]] == FlowMapWeights[CurrentCellIndex])
				OutputVertical = 2 * (FlowMapWeights[Neighbors[2]] - FlowMapWeights[Neighbors[3]]);
			else
				OutputVertical = (FlowMapWeights[Neighbors[2]] - FlowMapWeights[Neighbors[3]]);

			//Get our vector from those components
			OutputVector = (FVector2D{OutputHorizontal, OutputVertical}.GetSafeNormal());

			FCollisionQueryParams QueryParams;
			QueryParams.AddIgnoredActor(Target);

			FVector IndexRelativeFloorLocation = LineTraceWithinVolume(GetIndexRelativeCoordinates(CurrentCellIndex) + FVector2D{ FlowMapOrigin }, ECC_WorldStatic, QueryParams).Location;
			PlayerFlowmapFloorHeight[CurrentCellIndex] = IndexRelativeFloorLocation.Z;

			// Draw Debug
			if (DrawDebug)
			{

				FHitResult HitResult;

				FBoxSphereBounds ComponentBounds = BoundingBox->Bounds;
				float MaxHeight = ComponentBounds.Origin.Z + ComponentBounds.BoxExtent.Z;
				
				FVector StartLocation = (FVector{ GetIndexRelativeCoordinates(CurrentCellIndex), MaxHeight } + FVector{FlowMapOrigin.X, FlowMapOrigin.Y, 0});
				FVector EndLocation = StartLocation - FVector{ 0, 0, ComponentBounds.BoxExtent.Z * 2 };
				FCollisionQueryParams DebugQueryParams;

				GetWorld()->LineTraceSingleByChannel(
					HitResult,
					StartLocation,
					EndLocation,
					ECC_WorldStatic,
					DebugQueryParams
				);

				/*
				DrawDebugLine(
					GetWorld(),
					StartLocation,
					EndLocation,
					FColor::Green,
					false,
					1.0f,
					0,
					2.0f
				);
				*/
			
				FVector DebugStart = (FVector{ GetIndexRelativeCoordinates(CurrentCellIndex), HitResult.Location.Z + 15 } + FVector{ FlowMapOrigin.X, FlowMapOrigin.Y, 0});
				FVector DebugEnd = (FVector{ GetIndexRelativeCoordinates(CurrentCellIndex) + (OutputVector * Resolution), HitResult.Location.Z + 15 } + FVector{ FlowMapOrigin.X, FlowMapOrigin.Y, 0 });

				//Draw Directional arraws displaying the flowmap
				DrawDebugDirectionalArrow(
					GetWorld(),
					DebugStart,
					DebugEnd,
					50.0f,
					FColor::Red,
					false,
					0.2f,
					0,
					5.0f
				);				
			}

			//Finally, set the flowmap value at index

			FlowMap[CurrentCellIndex] = FVector {OutputVector, 0 };
		}
		*Array = FlowMap;
	}
}

void ASwarmManager::UpdateFlowMaps()
{
	GenerateFlowMap(UGameplayStatics::GetPlayerCharacter(GetWorld(), 0), &PlayerFlowmap);
}

void ASwarmManager::OnOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("New rat added"));
	if (Cast<ARatCPPBase>(OtherActor))
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("New rat added"));
	}
}

//The main function that moves the units every tick
void ASwarmManager::MoveUnits(float DeltaTime)
{
	DeltaTime = FMath::Min(DeltaTime, 0.05f); //Clamp delta-time to prevent window out-of-focus number explosions

	if (PlayerFlowmap.Num() == 0) return;

	int InstanceCount = EnemyInstancedMesh->GetInstanceCount();

	//Static arrays save performance
	static TArray<FTransform> ArrayOfTransforms;
	ArrayOfTransforms.SetNumUninitialized(InstanceCount);

	static TArray<int32> RatCellIDs;
	RatCellIDs.SetNumUninitialized(InstanceCount);

	// Reset collision groups
	RatCollisionGroups.SetNum(PlayerFlowmap.Num());
	for (int b = 0; b < RatCollisionGroups.Num(); b++)
	{
		RatCollisionGroups[b].Reset(); //Reset instead of empty to save performance
	}

	//Calculate the reciporocal to avoid division in loops
	float InvResolution = 1.0f / Resolution;
	int32 WidthInCells = FMath::FloorToInt((width * 2.0f) * InvResolution);

	int32 FlowmapCount = PlayerFlowmap.Num(); //Cache the loop limit just in case to save performance

	for (int i = 0; i < InstanceCount; i++)
	{
		//Manual inline of GetFlowmapIndex to avoid function call overhead
		FVector RelativeLoc = RatData[i].Location - FlowMapOrigin;
		int32 XIndex = FMath::FloorToInt(RelativeLoc.X * InvResolution);
		int32 YIndex = FMath::FloorToInt(RelativeLoc.Y * InvResolution);
		int32 RoundedIndex = XIndex + (YIndex * WidthInCells);

		//Add this rat to its appropriate collision group
		if (RoundedIndex >= 0 && RoundedIndex < FlowmapCount)
		{
			RatCellIDs[i] = RoundedIndex;
			RatCollisionGroups[RoundedIndex].Add(i);
		}
		else
		{
			RatCellIDs[i] = -1;
		}
	}

	//Steering/Collision controls
	float SeparationRadius = EnemyInstancedMesh->GetStaticMesh()->GetBounds().BoxExtent.GetMin() * 15.0f;
	float SeparationRadiusSq = SeparationRadius * SeparationRadius;
	float MoveSpeed = 500.0f;
	float MaxSpeedSq = (MoveSpeed * 2.5f) * (MoveSpeed * 2.5f);
	float SepMultiplier = MoveSpeed * 5.0f;
	float InterpAlpha = FMath::Clamp(DeltaTime * 15.0f, 0.0f, 1.0f);
	float GravityStep = 980.0f * DeltaTime;

	// Calculate Steering & Movement
	for (int32 i = 0; i < InstanceCount; i++)
	{
		int ThisRatsArrayID = RatCellIDs[i];
		FVector CurrentLocation = RatData[i].Location;
		FVector Velocity = RatData[i].Velocity;
		FVector TargetVelocity = FVector::ZeroVector;

		if (ThisRatsArrayID != -1)
		{
			TargetVelocity = PlayerFlowmap[ThisRatsArrayID] * MoveSpeed;

			const TArray<int32>& Group = RatCollisionGroups[ThisRatsArrayID];
			int GroupSize = Group.Num();

			if (GroupSize > 1) //Check against neighbors
			{
				int CheckedNeighbors = 0;
				int Step = FMath::Max(1, GroupSize / 8); //Step helps us check a good sample size of rats without checking every one, in case many are crammed in one cell
				float SepForceX = 0.0f;
				float SepForceY = 0.0f;

				for (int c = 0; c < GroupSize; c += Step)
				{
					if (CheckedNeighbors >= 6) break;

					int OtherRatID = Group[c];
					if (OtherRatID != i)
					{
						//Calculate 2D distance manually (bypasses FVector overhead)
						float dX = CurrentLocation.X - RatData[OtherRatID].Location.X;
						float dY = CurrentLocation.Y - RatData[OtherRatID].Location.Y;
						float DistSq = (dX * dX) + (dY * dY);

						//Seperation force math
						if (DistSq < SeparationRadiusSq && DistSq > 1.0f)
						{
							float InvDist = FMath::InvSqrt(DistSq);
							float ActualDist = DistSq * InvDist;
							float ForceMag = ((SeparationRadius - ActualDist) / SeparationRadius) * SepMultiplier * InvDist;

							SepForceX += dX * ForceMag;
							SepForceY += dY * ForceMag;
						}
						CheckedNeighbors++;
					}
				}
				TargetVelocity.X += SepForceX;
				TargetVelocity.Y += SepForceY;
			}
		}

		//Fast Clamp
		float TargetVelSq = (TargetVelocity.X * TargetVelocity.X) + (TargetVelocity.Y * TargetVelocity.Y);
		if (TargetVelSq > MaxSpeedSq)
		{
			float InvVel = FMath::InvSqrt(TargetVelSq);
			TargetVelocity.X *= (MoveSpeed * 1.5f) * InvVel;
			TargetVelocity.Y *= (MoveSpeed * 1.5f) * InvVel;
		}

		float FloorHeight = (ThisRatsArrayID != -1) ? PlayerFlowmapFloorHeight[ThisRatsArrayID] : 0.0f;

		//Manual Lerp to completely bypass VInterpTo overhead
		Velocity.X += (TargetVelocity.X - Velocity.X) * InterpAlpha;
		Velocity.Y += (TargetVelocity.Y - Velocity.Y) * InterpAlpha;
		Velocity.Z = (CurrentLocation.Z >= FloorHeight) ? (Velocity.Z - GravityStep) : 0.0f;

		CurrentLocation.X += Velocity.X * DeltaTime;
		CurrentLocation.Y += Velocity.Y * DeltaTime;
		CurrentLocation.Z += Velocity.Z * DeltaTime;

		//Snap to floor height if we end below it
		if (CurrentLocation.Z < FloorHeight)
		{
			CurrentLocation.Z = FloorHeight;
			Velocity.Z = 0.0f;
		}

		RatData[i].Velocity = Velocity;
		RatData[i].Location = CurrentLocation;

		//Rotation math
		float Yaw = 0.0f;
		if (TargetVelSq > 1.0f)
		{
			Yaw = FMath::RadiansToDegrees(FMath::Atan2(Velocity.Y, Velocity.X) - 90);
		}

		ArrayOfTransforms[i] = FTransform(FRotator(0.0f, Yaw, 0.0f), CurrentLocation, FVector::OneVector);
	}

	//Ship rat positions to GPU
	EnemyInstancedMesh->BatchUpdateInstancesTransforms(0, ArrayOfTransforms, true, false, true);
}
