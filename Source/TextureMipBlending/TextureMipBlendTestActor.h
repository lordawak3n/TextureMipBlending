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

/** How RefineBias is applied when swapping textures (Phase 2 vs Phase 3). */
UENUM(BlueprintType)
enum class ETextureMipBlendSwapMode : uint8
{
	/** RefineBias always 0 — expect green->red pop on hi-res upgrade (Phase 2). */
	Naive UMETA(DisplayName = "Naive (Bias = 0)"),

	/** On hi-res bind, RefineMinMip = log2(hi/lo) and RefineBias = 0 (Phase 3). Requires material MinMip shader. */
	BiasFrozenAtUpgrade UMETA(DisplayName = "Min Mip Frozen At Upgrade"),

	/** Animate RefineMinMip down to 0 after hi-res bind (Phase 4). */
	Fade UMETA(DisplayName = "Fade (MinMip to 0)")
};

/**
 * Persistent terrain-tile stand-in for the mip-bias refine experiment.
 *
 * Phase 1: honest lo-res / hi-res pair, lo-res bound at start.
 * Phase 2: dedicated keys bind lo/hi textures; SwapRefineMode = Naive (RefineBias 0, pop).
 * Phase 4: SwapRefineMode = Fade animates RefineMinMip from log2(hi/lo) down to 0.
 * Phase 5a: checker on finest mips; duplicate actor + default-sampler material for aniso compare.
 *
 * Material contract (M_TileMipBlend):
 *   Texture param   "TileTexture"
 *   Scalar param    "RefineBias"    (optional offset; keep 0 for frozen mode)
 *   Scalar param    "RefineMinMip"  (clamp: mip = max(LOD, RefineMinMip) — see material setup)
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

	/** World size of the tile plane in centimeters. Large enough that close and far pixels pick different mips. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Tile", meta = (ClampMin = "100.0"))
	float TileExtentCm = 2000.0f;

	/** Finest mip of the starting texture (power of two). Hi-res is always 2x this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Textures", meta = (ClampMin = "16", ClampMax = "4096"))
	int32 LoResSize = 512;

	/**
	 * Phase 5a: every mip uses a color-coded checker (same cell count per mip edge).
	 * Hi-res mip 0 uses 2x texels/cell so its UV frequency matches mip 1 / lo-res finest.
	 */
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

	/** Controls RefineBias behavior when swapping lo-res <-> hi-res. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Refine")
	ETextureMipBlendSwapMode SwapRefineMode = ETextureMipBlendSwapMode::BiasFrozenAtUpgrade;

	/**
	 * Duration of the RefineBias +1 -> 0 fade after a hi-res bind (SwapRefineMode = Fade only).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Mip Blend|Refine",
		meta = (ClampMin = "0.0", EditCondition = "SwapRefineMode == ETextureMipBlendSwapMode::Fade", EditConditionHides))
	float FadeDurationSeconds = 0.5f;

	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category = "Texture Mip Blend|Debug",
		meta = (DisplayName = "Upgrade Refine Bias"))
	float UpgradeRefineBias = 1.0f;

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
		meta = (DisplayName = "Refine Bias"))
	float CurrentRefineBias = 0.0f;

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
	void BindTextureToMaterial(UTexture2D* Texture, bool bIsHiResBinding, bool bForceRecreateMid, float RefineBias, float RefineMinMip);
	void ApplyRefineScalarsToMid(float RefineBias, float RefineMinMip);
	bool MaterialSupportsRefineScalars() const;
	void StartRefineMinMipFade();
	void StopRefineMinMipFade();
	float GetUpgradeRefineBias() const;
	int32 GetCheckerCellsPerEdge() const;
	int32 GetCheckerTexelsPerCellForMipSize(int32 MipSize) const;
	void ComputeRefineScalarsForBind(bool bIsHiResBinding, float& OutRefineBias, float& OutRefineMinMip) const;
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
