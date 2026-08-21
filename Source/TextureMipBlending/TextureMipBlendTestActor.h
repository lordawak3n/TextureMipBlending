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
 * Persistent terrain-tile stand-in for the mip refine fade experiment.
 *
 * Hi-res bind starts at RefineMinMip = log2(hi/lo), then fades RefineMinMip to 0.
 *
 * Material contract (M_TileMipBlend):
 *   Texture param   "TileTexture"
 *   Scalar param    "RefineMinMip"  (clamp: mip = max(LOD, RefineMinMip))
 */
UCLASS(Blueprintable, BlueprintType)
class TEXTUREMIPBLENDING_API ATextureMipBlendTestActor : public AActor
{
	GENERATED_BODY()

public:
	ATextureMipBlendTestActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Assign /Game/Materials/M_TileMipBlend after you create it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Material")
	TObjectPtr<UMaterialInterface> TileMaterial;

	/** Scale applied to the engine plane mesh (100uu base → X/Y cm when Z=1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Tile")
	FVector TileMeshScale = FVector(20.0f, 20.0f, 1.0f);

	/** Finest mip of the starting texture (power of two). Hi-res is always 2x this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures", meta = (ClampMin = "16", ClampMax = "4096"))
	int32 LoResSize = 512;

	/** Every mip uses a color-coded checker (same cell count per mip edge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures")
	bool bCheckerOnFinestMips = true;

	/** Checker cell size in texels at lo-res finest mip; coarser mips scale to keep cell count constant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures",
		meta = (ClampMin = "2", ClampMax = "256", EditCondition = "bCheckerOnFinestMips", EditConditionHides))
	int32 CheckerTexelsPerCell = 32;

	/** Requested max anisotropy — applied via r.MaxAnisotropy at BeginPlay (UE 5.6 has no per-texture override). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxTextureAnisotropy = 8;

	/** Short label for Output Log (e.g. "Custom SampleLevel" vs "Default Aniso"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Debug")
	FString SamplingLabel = TEXT("Custom");

	/** Seconds to wait after a bind key before the texture actually changes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input", meta = (ClampMin = "0.0"))
	float SwapDelaySeconds = 0.5f;

	/** Bind lo-res texture (after SwapDelaySeconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input")
	FKey LoResKey = EKeys::One;

	/** Bind hi-res texture (after SwapDelaySeconds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input")
	FKey HiResKey = EKeys::Two;

	/** When true, lo/hi bind keys update every synced actor in the level (side-by-side compare planes). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Input")
	bool bSyncTextureBindsWithGroup = true;

	/** Duration of the RefineMinMip fade after a hi-res bind (log2(hi/lo) down to 0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Refine", meta = (ClampMin = "0.0"))
	float FadeDurationSeconds = 0.5f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Hi Res Bind Refine Min Mip"))
	float UpgradeRefineMinMip = 1.0f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Refine Fade Active"))
	bool bRefineFadeActive = false;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Bound To Hi Res"))
	bool bBoundToHiRes = false;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Swap Scheduled"))
	bool bSwapScheduled = false;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Refine Min Mip"))
	float CurrentRefineMinMip = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Bound Texture"))
	FString BoundTextureDescription;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Lo Res Texture Object"))
	TObjectPtr<UTexture2D> LoResTexture;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Hi Res Texture Object"))
	TObjectPtr<UTexture2D> HiResTexture;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Tile MID"))
	TObjectPtr<UMaterialInstanceDynamic> TileMid;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	TArray<FMipDebugColorEntry> HiResMipColorLegend;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	TArray<FMipDebugColorEntry> LoResMipColorLegend;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug|Mip Colors")
	FString MipColorLegendSummary;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Texture Mip Blend|Tile")
	TObjectPtr<UStaticMeshComponent> TileMesh;

private:
	void InitializeTileVisuals(bool bForceTextureRebuild);
	void ApplyTileTransform();
	void EnsureGeneratedTextures(bool bForceRebuild);
	void BindTextureToMaterial(UTexture2D* Texture, bool bIsHiResBinding, bool bForceRecreateMid, float RefineMinMip);
	void ApplyRefineMinMipToMid(float RefineMinMip);
	bool MaterialSupportsRefineMinMip() const;
	void StartRefineMinMipFade();
	void StopRefineMinMipFade();
	float GetUpgradeRefineMinMip() const;
	float GetRefineMinMipForBind(bool bIsHiResBinding) const;
	int32 GetCheckerCellsPerEdge() const;
	int32 GetCheckerTexelsPerCellForMipSize(int32 MipSize) const;
	void LogMaterialScalarParameterNames() const;
	void EnsureTextureResourceReady(UTexture2D* Texture);
	void LogMaterialTextureParameterNames() const;
	void RefreshMipColorDebugInfo();
	void SetupSwapInput();
	void RegisterWithBindGroup();
	void UnregisterFromBindGroup();
	bool IsBindInputLeader() const;
	void TryAcquireBindInputLeader();
	void BroadcastBindRequest(bool bBindHiRes, const FKey& SourceKey);
	void RequestTextureBind(bool bBindHiRes, const FKey& SourceKey);
	void HandleLoResKeyPressed();
	void HandleHiResKeyPressed();
	void ApplyPendingTextureBind();

	bool bPendingBindHiRes = false;
	float RefineFadeElapsedSeconds = 0.0f;
	float RefineFadeStartMinMip = 0.0f;
	FTimerHandle SwapDelayTimerHandle;

	UTexture2D* CreateSolidMipTexture(int32 SizeX, FName DebugName);
	void FillMipSolidColor(UTexture2D* Texture, int32 MipIndex, FColor Color);
	void FillMipCheckerPattern(UTexture2D* Texture, int32 MipIndex, FColor BaseColor, int32 TexelsPerCell);
	void CopyMip(UTexture2D* Dest, int32 DestMipIndex, UTexture2D* Source, int32 SourceMipIndex);
	static FColor GetMipDebugColor(int32 MipIndexFromFinest);
	static FColor GetMipCheckerAlternateColor(FColor BaseColor);
	static FName GetMipDebugColorName(int32 MipIndexFromFinest);
	static FString GetMipDebugDisplayName(int32 MipIndexFromFinest, bool bChecker);
	static int32 CountMips(int32 SizeX);
};
