// Fill out your copyright notice in the Description page of Project Settings.

#include "SwarmManager.h"
#include "RatCPPBase.h"
#include "GameFramework/Character.h"
#include "Components/BoxComponent.h"
#include "Math/UnrealMathUtility.h"
#include "Kismet/GameplayStatics.h"
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

	//Pre-allocate the rat data arrays, 3000 is hardcoded, in the future this should be a variable
	RatData.SetNum(3000);
	RatRenderData.SetNum(3000);

	//Spawn all Instanced Static Meshes. Again, 3000 is hardcoded for now
	for (int32 i = 0; i < 3000; i++)
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
	int NumberOfPoints = ((2 * width) / Resolution) * ((2 * depth) / Resolution);

	//Pre-allocate that many points in the array
	TArray<FVector> FlowMap;
	FlowMap.SetNum(NumberOfPoints);

	//Initialize them. FLT_MAX is used as a placeholder for a point that needs to be updated
	TArray<float> FlowMapWeights;
	FlowMapWeights.Init(FLT_MAX, NumberOfPoints);

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

		int WidthInCells = (2 * width) / Resolution;
		int DepthInCells = (2 * depth) / Resolution;

		//Pre-allocate the heightmap array
		PlayerFlowmapFloorHeight.SetNum(WidthInCells * DepthInCells + 1);

		//Convert those coordinates into an array index
		TargetCellIndex = (TargetCellXCoord) + (TargetCellYCoord * WidthInCells);

		//Helper function, returns the indeces of the orthogonal neigbors of a cell
		auto GetNeighborIndeces = [&](int Index) -> TArray<float>
		{
			TArray<float> Indeces;
			Indeces.Init(Index, 4);
			if ((Index - 1 >= 0) && ((Index) % WidthInCells != 0))
				Indeces[0] = (Index - 1);
			if ((Index + 1 < NumberOfPoints) && ((Index + 1) % WidthInCells != 0))
				Indeces[1] = (Index + 1);
			if (Index - WidthInCells > 0)
				Indeces[2] = (Index - WidthInCells);
			if (Index + WidthInCells < NumberOfPoints)
				Indeces[3] = (Index + WidthInCells);
			return Indeces;
		};

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

					OutputArray[i] = average;
				}

				*FlowMap = OutputArray;
			};

		//Run the gaussian blur a few times. Tweak this to change flowmap smoothness, higher values impact performance
		for (int i = 0; i < 10; i++)
		{
			GaussianBlur(&FlowMapWeights);
		}
	
		//Generate flowmap vectors from flowmap weights
		for (int32 CurrentCellIndex = 0; CurrentCellIndex < FlowMapWeights.Num(); CurrentCellIndex++)
		{
			//Again, using MAX_32 as a placeholder
			if (FlowMapWeights[CurrentCellIndex] == MAX_int32) continue;

			if (FlowMapWeights[CurrentCellIndex] == 0)
			{
				FlowMap[CurrentCellIndex] = FVector::ZeroVector; // No flow at the target
				continue;
			}

			FVector2D OutputVector;

			float OutputHorizontal;
			float OutputVertical;

			//Get the X and Y components of our vector, with edge-case handling
			if (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[0]] == FlowMapWeights[CurrentCellIndex] || FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[1]] == FlowMapWeights[CurrentCellIndex])
				OutputHorizontal = 2 * (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[0]] - FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[1]]);
			else
				OutputHorizontal = (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[0]] - FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[1]]);

			if (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[2]] == FlowMapWeights[CurrentCellIndex] || FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[3]] == FlowMapWeights[CurrentCellIndex])
				OutputVertical = 2 * (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[2]] - FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[3]]);
			else
				OutputVertical = (FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[2]] - FlowMapWeights[GetNeighborIndeces(CurrentCellIndex)[3]]);

			//Get our vector from those components
			OutputVector = (FVector2D{OutputHorizontal, OutputVertical}.GetSafeNormal());

			//Converts an array index back into local-space coordinates
			auto GetIndexRelativeCoordinates = [&](int Index) -> FVector2D
				{
					FVector2D OutputVector = { (Index % WidthInCells) * Resolution, (Index / WidthInCells) * Resolution };
					return OutputVector;
				};

			//Find the height at this flowmap index
			FVector IndexRelativeFloorLocation = LineTraceWithinVolume(GetIndexRelativeCoordinates(CurrentCellIndex) + FVector2D{ FlowMapOrigin }, ECC_WorldStatic, FCollisionQueryParams()).Location;
			PlayerFlowmapFloorHeight[CurrentCellIndex] = IndexRelativeFloorLocation.Z;

			// Draw Debug

			if (DrawDebug)
			{

				FHitResult HitResult;

				FBoxSphereBounds ComponentBounds = BoundingBox->Bounds;
				float MaxHeight = ComponentBounds.Origin.Z + ComponentBounds.BoxExtent.Z;

				
				FVector StartLocation = (FVector{ GetIndexRelativeCoordinates(CurrentCellIndex), MaxHeight } + FVector{FlowMapOrigin.X, FlowMapOrigin.Y, 0});
				FVector EndLocation = StartLocation - FVector{ 0, 0, ComponentBounds.BoxExtent.Z * 2 };
				FCollisionQueryParams QueryParams;

				GetWorld()->LineTraceSingleByChannel(
					HitResult,
					StartLocation,
					EndLocation,
					ECC_WorldStatic,
					QueryParams
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
	//GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("Hello World! I'm Properly Debugging"));
}

void ASwarmManager::OnOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("New rat added"));
	if (Cast<ARatCPPBase>(OtherActor))
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("New rat added"));
	}
}

void ASwarmManager::MoveUnits(float DeltaTime)
{
	if (PlayerFlowmap.Num() == 0) return;

	int InstanceCount = EnemyInstancedMesh->GetInstanceCount();
	TArray<FTransform> ArrayOfTransforms;
	ArrayOfTransforms.SetNumUninitialized(InstanceCount);

	// Reset buckets
	RatCollisionGroups.SetNum(PlayerFlowmap.Num());
	for (int b = 0; b < RatCollisionGroups.Num(); b++)
	{
		RatCollisionGroups[b].Reset();
	}

	//Loop through all rats, putting them in a "collision group" with other rats close to their index
	for (int i = 0; i < InstanceCount; i++)
	{
		int ThisRatsArrayID = GetFlowmapIndexAtWorldLocation(RatData[i].Location);
		if (ThisRatsArrayID != -1)
		{
			RatCollisionGroups[ThisRatsArrayID].Add(i);
		}
	}

	FVector BaseMeshBounds = EnemyInstancedMesh->GetStaticMesh()->GetBounds().BoxExtent;
	float CollisionRadius = BaseMeshBounds.GetMin();
	float SeparationRadius = CollisionRadius * 10.0f; // Distance where separation kicks in
	float MoveSpeed = 500.0f; // Base movement speed in cm/s

	//Calculate Steering & Movement
	for (int i = 0; i < InstanceCount; i++)
	{
		int ThisRatsArrayID = GetFlowmapIndexAtWorldLocation(RatData[i].Location);
		FVector CurrentLocation = RatData[i].Location;

		//FLOWMAP FORCE (Desire to reach player)
		FVector FlowDirection = (ThisRatsArrayID != -1) ? PlayerFlowmap[ThisRatsArrayID] : FVector::ZeroVector;
		FVector FlowVelocity = FlowDirection * MoveSpeed;

		//SEPARATION FORCE (Desire to stay away from neighbors)
		FVector SeparationForce = FVector::ZeroVector;

		if (ThisRatsArrayID != -1)
		{
			//Find all other rats in this rat's collision group
			for (int c = 0; c < RatCollisionGroups[ThisRatsArrayID].Num(); c++)
			{
				int OtherRatID = RatCollisionGroups[ThisRatsArrayID][c];
				if (OtherRatID != i)
				{
					//Work strictly in 2D (ignore Z height for separation)
					FVector MyLoc2D = FVector(CurrentLocation.X, CurrentLocation.Y, 0.0f);
					FVector OtherLoc2D = FVector(RatData[OtherRatID].Location.X, RatData[OtherRatID].Location.Y, 0.0f);

					float Dist = FVector::Dist(MyLoc2D, OtherLoc2D);

					if (Dist < SeparationRadius && Dist > 0.1f)
					{
						FVector PushDir = (MyLoc2D - OtherLoc2D).GetSafeNormal();

						//Inverse weighting: The closer they are, the exponentially stronger the push force
						float Strength = (SeparationRadius - Dist) / SeparationRadius;
						SeparationForce += (PushDir * Strength * MoveSpeed * 3);
					}
				}
			}
		}

		//COMBINE FORCES (Flow Vector + Separation Vector)
		FVector TargetVelocity = FlowVelocity + SeparationForce;

		//Clamp max speed so separation forces don't launch rats across the map
		TargetVelocity = TargetVelocity.GetClampedToMaxSize(MoveSpeed * 1.5f);

		//Keep vertical gravity intact
		float FloorHeight = (ThisRatsArrayID != -1) ? PlayerFlowmapFloorHeight[ThisRatsArrayID] : 0.0f;
		float NewVelocityZ = (CurrentLocation.Z >= FloorHeight) ? (RatData[i].Velocity.Z - (980.0f * DeltaTime)) : 0.0f;

		//SMOOTH INTERPOLATION (VInterpTo eliminates all jitter!)
		//InterpSpeed (15.0f) controls responsiveness. Higher = tighter, Lower = smoother/heavier
		FVector SmoothedXYVelocity = FMath::VInterpTo(
			FVector(RatData[i].Velocity.X, RatData[i].Velocity.Y, 0.0f),
			FVector(TargetVelocity.X, TargetVelocity.Y, 0.0f),
			DeltaTime,
			15.0f
		);

		//Apply final smoothed velocity
		RatData[i].Velocity.X = SmoothedXYVelocity.X;
		RatData[i].Velocity.Y = SmoothedXYVelocity.Y;
		RatData[i].Velocity.Z = NewVelocityZ;

		//INTEGRATE POSITION (Location += Velocity * DeltaTime)
		FVector NewLocation = CurrentLocation + (RatData[i].Velocity * DeltaTime);

		//Snap to floor if below terrain
		if (NewLocation.Z < FloorHeight)
		{
			NewLocation.Z = FloorHeight;
			RatData[i].Velocity.Z = 0.0f;
		}

		//Save Location
		RatData[i].Location = NewLocation;

		//Update GPU Instance Transform
		//Set rotation to match movement direction for smooth turning
		FRotator TargetRotation = RatData[i].Velocity.IsNearlyZero() ? FRotator::ZeroRotator : RatData[i].Velocity.Rotation();
		ArrayOfTransforms[i] = FTransform(TargetRotation, NewLocation, FVector::OneVector);
	}

	//Ship to GPU
	EnemyInstancedMesh->BatchUpdateInstancesTransforms(0, ArrayOfTransforms, true, true);
}