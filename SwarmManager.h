// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RatCPPBase.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "SwarmManager.generated.h"

class UBoxComponent;

class UInstancedStaticMeshComponent;

USTRUCT()
struct FRatData
{
	GENERATED_BODY()

	FVector Location;
	FVector Velocity;
	int FlowmapIndexCoord;
};

UCLASS()
class MYPROJECT2_API ASwarmManager : public AActor
{
	GENERATED_BODY()
	
public:	
	
	//Arrays containing entity data for Instanced Static Mesh

	TArray<FRatData> RatData;
	TArray<TArray<int>> RatCollisionGroups;
	TArray<FTransform> RatRenderData;
	
	//Constructor
	ASwarmManager();

	// Bounding box
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	UBoxComponent* BoundingBox;

	//Flow Map, an array of vectors pointing to the player
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flowmaps")
	TArray<FVector> PlayerFlowmap;

	//The height at each point in the flowmap
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flowmaps")
	TArray<float> PlayerFlowmapFloorHeight;

	//Array of units this Manager controlls
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TArray<ARatCPPBase*> ControlledUnits;

	//Overlap function
	UFUNCTION()
	void OnOverlapBegin(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	//Run every tick to move units
	UFUNCTION()
	void MoveUnits(float DeltaTime);

	//Instanced Static Mesh
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> EnemyInstancedMesh;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	// Point grid resolution
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Components")
	float Resolution = 50;

	// How often to update flowmaps (in seconds)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Components")
	float FlowMapTickRate = 0.2;

	// Draw Debug
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Debug")
	bool DrawDebug = true;

	// This function will be called when the FlowMapUpdate timer elapses
	UFUNCTION()
	void UpdateFlowMaps();

	//Helper function that does a line trace from the top of the Manager's volume to the bottom
	FHitResult LineTraceWithinVolume(FVector2d CoordsInBounds, ECollisionChannel CollisionChannel, FCollisionQueryParams QueryParams);

	//Helper function that converts world-space locations to the nearest index in the manager's flowmap
	int GetFlowmapIndexAtWorldLocation(FVector Location);

	//The origin point of the flowmap
	FVector FlowMapOrigin;

	//don't touch these
	int width;
	int depth;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	//Flowmap generation function
	void GenerateFlowMap(AActor* Target, TArray<FVector>* Array);

	// A handle to reference and control the active timer
	FTimerHandle FlowMapUpdate;

};
