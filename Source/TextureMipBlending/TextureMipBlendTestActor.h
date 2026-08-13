#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "TextureMipBlendTestActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;
class UTexture2D;

/** One row in the mip color legend shown in the actor details panel. */
USTRUCT(BlueprintType)
struct FMipDebugColorEntry
{
	GENERATED_BODY()

	/** 0 = full resolution (mip 0), 1 = half resolution, etc. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mip")
	int32 MipIndex = 0;

	/** Width/height of this mip in texels (square mips). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mip")
	int32 MipSizeTexels = 0;

	/** Human-readable color name (Red, Green, Blue, ...). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mip")
	FString ColorName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mip")
	FColor Color = FColor::Black;
};

/**
 * Persistent terrain-tile stand-in for the mip-bias refine experiment.
 *
 * Phase 1: builds an honest lo-res / hi-res pair and binds only lo-res.
 * Texture swapping is not wired yet.
 *
 * Material contract (M_TileMipBlend):
 *   Texture param  "TileTexture"
 *   Scalar param   "RefineBias"
 */
UCLASS(Blueprintable, BlueprintType)
class TEXTUREMIPBLENDING_API ATextureMipBlendTestActor : public AActor
{
	GENERATED_BODY()

public:
	ATextureMipBlendTestActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Assign /Game/Materials/M_TileMipBlend after you create it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Material")
	TObjectPtr<UMaterialInterface> TileMaterial;

	/** World size of the tile plane in centimeters. Large enough that close and far pixels pick different mips. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Tile", meta = (ClampMin = "100.0"))
	float TileExtentCm = 2000.0f;

	/** Finest mip of the starting texture (power of two). Hi-res is always 2x this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures", meta = (ClampMin = "16", ClampMax = "4096"))
	int32 LoResSize = 512;

	/**
	 * Seconds to wait after the swap key before the bound texture actually changes.
	 * Not used until the swap milestone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input", meta = (ClampMin = "0.0"))
	float SwapDelaySeconds = 0.5f;

	/**
	 * Same key upgrades lo-res -> hi-res and toggles back.
	 * Not used until the swap milestone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input")
	FKey SwapKey = EKeys::One;

	/**
	 * Duration of the RefineBias +1 -> 0 fade after a hi-res bind.
	 * Not used until the fade milestone.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Fade", meta = (ClampMin = "0.0"))
	float FadeDurationSeconds = 0.5f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Bound Texture"))
	FString BoundTextureDescription;

	/** Runtime-generated 512 (default) texture; lo-res finest mip is green. Not used in play until BeginPlay rebuilds it. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Lo Res Texture Object"))
	TObjectPtr<UTexture2D> LoResTexture;

	/** Runtime-generated 1024 (default) texture; includes red mip 0 that lo-res does not have. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Hi Res Texture Object"))
	TObjectPtr<UTexture2D> HiResTexture;

	/** Dynamic material instance on the plane; holds TileTexture + RefineBias at runtime. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Tile MID"))
	TObjectPtr<UMaterialInstanceDynamic> TileMid;

	/** Mip index -> solid debug color for HiResTexture (mip 0 = red, mip 1 = green, ...). */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	TArray<FMipDebugColorEntry> HiResMipColorLegend;

	/** Mip index -> color for LoResTexture (each row matches HiRes mip index + 1). */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	TArray<FMipDebugColorEntry> LoResMipColorLegend;

	/** One-line summary of the color key for the Output Log and details panel. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	FString MipColorLegendSummary;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Texture Mip Blend|Tile")
	TObjectPtr<UStaticMeshComponent> TileMesh;

private:
	void InitializeTileVisuals(bool bForceTextureRebuild);
	void ApplyTileTransform();
	void EnsureGeneratedTextures(bool bForceRebuild);
	void BindLoResToMaterial(bool bForceRecreateMid);
	void RefreshMipColorDebugInfo();

	UTexture2D* CreateSolidMipTexture(int32 SizeX, FName DebugName);
	void FillMipSolidColor(UTexture2D* Texture, int32 MipIndex, FColor Color);
	void CopyMip(UTexture2D* Dest, int32 DestMipIndex, UTexture2D* Source, int32 SourceMipIndex);
	static FColor GetMipDebugColor(int32 MipIndexFromFinest);
	static FName GetMipDebugColorName(int32 MipIndexFromFinest);
	static int32 CountMips(int32 SizeX);
};
