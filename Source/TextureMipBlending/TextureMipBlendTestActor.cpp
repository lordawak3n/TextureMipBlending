#include "TextureMipBlendTestActor.h"

#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTextureMipBlend, Log, All);

namespace TextureMipBlend
{
	static const FName TextureParamName(TEXT("TileTexture"));
	static const FName RefineBiasParamName(TEXT("RefineBias"));
	static const FName RefineMinMipParamName(TEXT("RefineMinMip"));
	static constexpr int32 BytesPerPixel = 4;
}

ATextureMipBlendTestActor::ATextureMipBlendTestActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	TileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TileMesh"));
	SetRootComponent(TileMesh);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane"));
	if (PlaneMesh.Succeeded())
	{
		TileMesh->SetStaticMesh(PlaneMesh.Object);
	}

	TileMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	TileMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	TileMesh->SetMobility(EComponentMobility::Movable);

	AutoReceiveInput = EAutoReceiveInput::Player0;
}

void ATextureMipBlendTestActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	ApplyTileTransform();

	// Super runs Blueprint Construction Script first — bind after so the mesh keeps our MID.
	if (GetWorld() && !GetWorld()->IsGameWorld())
	{
		InitializeTileVisuals(/*bForceTextureRebuild=*/true);
	}
}

void ATextureMipBlendTestActor::BeginPlay()
{
	Super::BeginPlay();

	bBoundToHiRes = false;
	bSwapScheduled = false;
	bPendingBindHiRes = false;
	bRefineFadeActive = false;
	RefineFadeElapsedSeconds = 0.0f;
	RefineFadeStartMinMip = 0.0f;
	CurrentRefineBias = 0.0f;
	CurrentRefineMinMip = 0.0f;

	// PIE duplicates can retain editor-world texture pointers or a MID that still samples the material default.
	InitializeTileVisuals(/*bForceTextureRebuild=*/true);
	SetupSwapInput();

	const TCHAR* ModeLabel = TEXT("MinMipFrozenAtUpgrade");
	if (SwapRefineMode == ETextureMipBlendSwapMode::Naive)
	{
		ModeLabel = TEXT("Naive");
	}
	else if (SwapRefineMode == ETextureMipBlendSwapMode::Fade)
	{
		ModeLabel = TEXT("Fade");
	}

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("%s Phase 4 ready: %dx%d lo-res bound. Mode=%s (UpgradeMinMip=%.2f). '%s'=lo-res, '%s'=hi-res (%.2fs delay)."),
		*GetName(),
		LoResTexture ? LoResTexture->GetSizeX() : 0,
		LoResTexture ? LoResTexture->GetSizeY() : 0,
		ModeLabel,
		UpgradeRefineBias,
		*LoResKey.GetDisplayName().ToString(),
		*HiResKey.GetDisplayName().ToString(),
		SwapDelaySeconds);
}

void ATextureMipBlendTestActor::Tick(float DeltaSeconds)
{
	if (!bRefineFadeActive || !bBoundToHiRes || !TileMid)
	{
		return;
	}

	if (FadeDurationSeconds <= 0.0f)
	{
		ApplyRefineScalarsToMid(0.0f, 0.0f);
		StopRefineMinMipFade();
		UE_LOG(LogTextureMipBlend, Display, TEXT("%s: RefineMinMip fade skipped (FadeDurationSeconds=0)."), *GetName());
		return;
	}

	RefineFadeElapsedSeconds += DeltaSeconds;
	const float Alpha = FMath::Clamp(RefineFadeElapsedSeconds / FadeDurationSeconds, 0.0f, 1.0f);
	const float EasedAlpha = FMath::SmoothStep(0.0f, 1.0f, Alpha);
	const float RefineMinMip = RefineFadeStartMinMip * (1.0f - EasedAlpha);

	ApplyRefineScalarsToMid(0.0f, RefineMinMip);

	if (Alpha >= 1.0f)
	{
		StopRefineMinMipFade();
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: RefineMinMip fade complete — close pixels should reach full hi-res sharpness (red)."),
			*GetName());
	}
}

void ATextureMipBlendTestActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopRefineMinMipFade();

	if (GetWorld())
	{
		GetWorldTimerManager().ClearTimer(SwapDelayTimerHandle);
	}

	bSwapScheduled = false;
	Super::EndPlay(EndPlayReason);
}

void ATextureMipBlendTestActor::SetupSwapInput()
{
	APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PlayerController)
	{
		EnableInput(PlayerController);
	}

	if (!InputComponent)
	{
		UE_LOG(LogTextureMipBlend, Warning, TEXT("%s has no InputComponent; bind keys will not work."), *GetName());
		return;
	}

	InputComponent->BindKey(LoResKey, IE_Pressed, this, &ATextureMipBlendTestActor::HandleLoResKeyPressed);
	InputComponent->BindKey(HiResKey, IE_Pressed, this, &ATextureMipBlendTestActor::HandleHiResKeyPressed);
}

void ATextureMipBlendTestActor::HandleLoResKeyPressed()
{
	RequestTextureBind(/*bBindHiRes=*/false, LoResKey);
}

void ATextureMipBlendTestActor::HandleHiResKeyPressed()
{
	RequestTextureBind(/*bBindHiRes=*/true, HiResKey);
}

void ATextureMipBlendTestActor::RequestTextureBind(bool bBindHiRes, const FKey& SourceKey)
{
	if (!GetWorld() || !LoResTexture || !HiResTexture)
	{
		return;
	}

	if (bSwapScheduled)
	{
		UE_LOG(LogTextureMipBlend, Warning,
			TEXT("%s: bind already scheduled (waiting %.2fs). Ignoring '%s'."),
			*GetName(),
			GetWorldTimerManager().GetTimerRemaining(SwapDelayTimerHandle),
			*SourceKey.GetDisplayName().ToString());
		return;
	}

	bPendingBindHiRes = bBindHiRes;

	if (SwapDelaySeconds <= 0.0f)
	{
		ApplyPendingTextureBind();
		return;
	}

	bSwapScheduled = true;
	float ExpectedBias = 0.0f;
	float ExpectedMinMip = 0.0f;
	ComputeRefineScalarsForBind(bPendingBindHiRes, ExpectedBias, ExpectedMinMip);

	const FString TargetLabel = bPendingBindHiRes
		? FString::Printf(TEXT("HiRes %dx%d (start RefineMinMip=%.2f)"), HiResTexture->GetSizeX(), HiResTexture->GetSizeY(), ExpectedMinMip)
		: FString::Printf(TEXT("LoRes %dx%d"), LoResTexture->GetSizeX(), LoResTexture->GetSizeY());

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("%s: '%s' pressed — binding %s in %.2fs."),
		*GetName(),
		*SourceKey.GetDisplayName().ToString(),
		*TargetLabel,
		SwapDelaySeconds);

	GetWorldTimerManager().SetTimer(
		SwapDelayTimerHandle,
		this,
		&ATextureMipBlendTestActor::ApplyPendingTextureBind,
		SwapDelaySeconds,
		false);
}

void ATextureMipBlendTestActor::ApplyPendingTextureBind()
{
	bSwapScheduled = false;
	GetWorldTimerManager().ClearTimer(SwapDelayTimerHandle);

	UTexture2D* TextureToBind = bPendingBindHiRes ? HiResTexture.Get() : LoResTexture.Get();
	if (!TextureToBind)
	{
		UE_LOG(LogTextureMipBlend, Error, TEXT("%s: pending texture bind failed — target texture is null."), *GetName());
		return;
	}

	StopRefineMinMipFade();

	const bool bWasLoRes = !bBoundToHiRes;
	float RefineBias = 0.0f;
	float RefineMinMip = 0.0f;
	ComputeRefineScalarsForBind(bPendingBindHiRes, RefineBias, RefineMinMip);
	BindTextureToMaterial(TextureToBind, bPendingBindHiRes, /*bForceRecreateMid=*/false, RefineBias, RefineMinMip);

	if (bPendingBindHiRes && SwapRefineMode == ETextureMipBlendSwapMode::Fade)
	{
		StartRefineMinMipFade();
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: hi-res bound — RefineMinMip fading %.2f -> 0 over %.2fs (close: green -> red)."),
			*GetName(),
			RefineFadeStartMinMip,
			FadeDurationSeconds);
	}
	else if (bWasLoRes && bBoundToHiRes)
	{
		if (SwapRefineMode == ETextureMipBlendSwapMode::Naive)
		{
			UE_LOG(LogTextureMipBlend, Display,
				TEXT("%s: NAIVE hi-res bind — close pixels should snap Green -> Red."),
				*GetName());
		}
		else
		{
			UE_LOG(LogTextureMipBlend, Display,
				TEXT("%s: hi-res bind RefineMinMip=%.2f — close pixels should stay Green."),
				*GetName(),
				CurrentRefineMinMip);
		}
	}
	else if (!bWasLoRes && !bBoundToHiRes)
	{
		UE_LOG(LogTextureMipBlend, Display, TEXT("%s: lo-res bound (RefineMinMip=0)."), *GetName());
	}
}

#if WITH_EDITOR
void ATextureMipBlendTestActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, LoResSize)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, TileMaterial)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, TileExtentCm))
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, LoResSize))
		{
			LoResTexture = nullptr;
			HiResTexture = nullptr;
			TileMid = nullptr;
		}

		InitializeTileVisuals(/*bForceTextureRebuild=*/true);
	}
}
#endif

void ATextureMipBlendTestActor::InitializeTileVisuals(bool bForceTextureRebuild)
{
	ApplyTileTransform();
	EnsureGeneratedTextures(bForceTextureRebuild);

	if (LoResTexture)
	{
		BindTextureToMaterial(LoResTexture, /*bIsHiResBinding=*/false, /*bForceRecreateMid=*/true, /*RefineBias=*/0.0f, /*RefineMinMip=*/0.0f);
	}
}

void ATextureMipBlendTestActor::ComputeRefineScalarsForBind(bool bIsHiResBinding, float& OutRefineBias, float& OutRefineMinMip) const
{
	OutRefineBias = 0.0f;
	OutRefineMinMip = 0.0f;

	switch (SwapRefineMode)
	{
	case ETextureMipBlendSwapMode::Naive:
		return;

	case ETextureMipBlendSwapMode::BiasFrozenAtUpgrade:
	case ETextureMipBlendSwapMode::Fade:
		if (bIsHiResBinding)
		{
			// Clamp finest mip: max(LOD, RefineMinMip). Constant MipBias +1 fails when LOD <= 0 on hi-res.
			OutRefineMinMip = GetUpgradeRefineBias();
			OutRefineBias = 0.0f;
		}
		return;

	default:
		return;
	}
}

float ATextureMipBlendTestActor::GetUpgradeRefineBias() const
{
	if (LoResSize <= 0)
	{
		return 0.0f;
	}

	const int32 HiResSize = LoResSize * 2;
	return FMath::Log2(static_cast<float>(HiResSize) / static_cast<float>(LoResSize));
}

void ATextureMipBlendTestActor::ApplyRefineScalarsToMid(float RefineBias, float RefineMinMip)
{
	if (!TileMid)
	{
		return;
	}

	const FMaterialParameterInfo BiasParamInfo(TextureMipBlend::RefineBiasParamName);
	const FMaterialParameterInfo MinMipParamInfo(TextureMipBlend::RefineMinMipParamName);

	TileMid->SetScalarParameterValueByInfo(BiasParamInfo, RefineBias);
	TileMid->SetScalarParameterValueByInfo(MinMipParamInfo, RefineMinMip);

	CurrentRefineBias = RefineBias;
	CurrentRefineMinMip = RefineMinMip;
}

void ATextureMipBlendTestActor::StartRefineMinMipFade()
{
	RefineFadeStartMinMip = CurrentRefineMinMip;
	RefineFadeElapsedSeconds = 0.0f;
	bRefineFadeActive = true;
	SetActorTickEnabled(true);
}

void ATextureMipBlendTestActor::StopRefineMinMipFade()
{
	bRefineFadeActive = false;
	RefineFadeElapsedSeconds = 0.0f;
	RefineFadeStartMinMip = 0.0f;
	SetActorTickEnabled(false);
}

void ATextureMipBlendTestActor::ApplyTileTransform()
{
	if (!TileMesh)
	{
		return;
	}

	// Engine plane is 100x100 uu (1m). Scale so the actor covers TileExtentCm on X and Y.
	const float UniformScale = TileExtentCm / 100.0f;
	TileMesh->SetRelativeScale3D(FVector(UniformScale, UniformScale, 1.0f));
}

int32 ATextureMipBlendTestActor::CountMips(int32 SizeX)
{
	int32 MipCount = 0;
	for (int32 Size = SizeX; Size >= 1; Size >>= 1)
	{
		++MipCount;
	}
	return MipCount;
}

FColor ATextureMipBlendTestActor::GetMipDebugColor(int32 MipIndexFromFinest)
{
	static const FColor MipColors[] =
	{
		FColor(220, 32, 32),    // 0 red    - hi-res finest (not on lo-res)
		FColor(32, 200, 48),    // 1 green  - lo-res finest / hi-res mip 1
		FColor(32, 80, 220),    // 2 blue
		FColor(230, 210, 32),   // 3 yellow
		FColor(200, 32, 200),   // 4 magenta (pink)
		FColor(32, 210, 210),   // 5 cyan
		FColor(230, 140, 32),   // 6 orange
		FColor(240, 240, 240),  // 7 white
		FColor(120, 120, 120),  // 8 grey
	};

	const int32 NumColors = UE_ARRAY_COUNT(MipColors);
	return MipColors[FMath::Clamp(MipIndexFromFinest, 0, NumColors - 1)];
}

FName ATextureMipBlendTestActor::GetMipDebugColorName(int32 MipIndexFromFinest)
{
	static const FName MipColorNames[] =
	{
		TEXT("Red"),
		TEXT("Green"),
		TEXT("Blue"),
		TEXT("Yellow"),
		TEXT("Magenta"),
		TEXT("Cyan"),
		TEXT("Orange"),
		TEXT("White"),
		TEXT("Grey"),
	};

	const int32 NumNames = UE_ARRAY_COUNT(MipColorNames);
	return MipColorNames[FMath::Clamp(MipIndexFromFinest, 0, NumNames - 1)];
}

void ATextureMipBlendTestActor::RefreshMipColorDebugInfo()
{
	HiResMipColorLegend.Reset();
	LoResMipColorLegend.Reset();
	MipColorLegendSummary.Reset();

	if (!HiResTexture)
	{
		return;
	}

	const int32 HiResMipCount = HiResTexture->GetNumMips();
	for (int32 MipIndex = 0; MipIndex < HiResMipCount; ++MipIndex)
	{
		FMipDebugColorEntry Entry;
		Entry.MipIndex = MipIndex;
		Entry.MipSizeTexels = HiResTexture->GetSizeX() >> MipIndex;
		Entry.ColorName = GetMipDebugColorName(MipIndex).ToString();
		Entry.Color = GetMipDebugColor(MipIndex);
		HiResMipColorLegend.Add(Entry);
	}

	if (LoResTexture)
	{
		const int32 LoResMipCount = LoResTexture->GetNumMips();
		for (int32 MipIndex = 0; MipIndex < LoResMipCount; ++MipIndex)
		{
			FMipDebugColorEntry Entry;
			Entry.MipIndex = MipIndex;
			Entry.MipSizeTexels = LoResTexture->GetSizeX() >> MipIndex;
			// Lo-res mip N is a byte copy of hi-res mip N+1 (honest server).
			const int32 SourceHiResMip = MipIndex + 1;
			Entry.ColorName = GetMipDebugColorName(SourceHiResMip).ToString();
			Entry.Color = GetMipDebugColor(SourceHiResMip);
			LoResMipColorLegend.Add(Entry);
		}
	}

	TArray<FString> SummaryParts;
	for (const FMipDebugColorEntry& Entry : HiResMipColorLegend)
	{
		SummaryParts.Add(FString::Printf(TEXT("mip%d=%s(%d)"), Entry.MipIndex, *Entry.ColorName, Entry.MipSizeTexels));
	}
	MipColorLegendSummary = FString::Join(SummaryParts, TEXT(", "));

	UE_LOG(LogTextureMipBlend, Display, TEXT("%s mip color key (HiRes): %s"), *GetName(), *MipColorLegendSummary);
}

UTexture2D* ATextureMipBlendTestActor::CreateSolidMipTexture(int32 SizeX, FName DebugName)
{
	UTexture2D* Texture = NewObject<UTexture2D>(this, DebugName, RF_Transient | RF_DuplicateTransient);
	Texture->SRGB = true;
	Texture->Filter = TF_Trilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->NeverStream = true;
	Texture->MipGenSettings = TMGS_LeaveExistingMips;
	Texture->CompressionSettings = TC_Default;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->bNotOfflineProcessed = true;

	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = SizeX;
	PlatformData->SizeY = SizeX;
	PlatformData->SetNumSlices(1);
	PlatformData->PixelFormat = PF_B8G8R8A8;
	Texture->SetPlatformData(PlatformData);

	for (int32 Size = SizeX; Size >= 1; Size >>= 1)
	{
		FTexture2DMipMap* Mip = new FTexture2DMipMap(Size, Size, 1);
		PlatformData->Mips.Add(Mip);

		const int64 Bytes = static_cast<int64>(Size) * Size * TextureMipBlend::BytesPerPixel;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		Mip->BulkData.Realloc(Bytes);
		Mip->BulkData.Unlock();
	}

	return Texture;
}

void ATextureMipBlendTestActor::FillMipSolidColor(UTexture2D* Texture, int32 MipIndex, FColor Color)
{
	if (!Texture || !Texture->GetPlatformData())
	{
		return;
	}

	TIndirectArray<FTexture2DMipMap>& Mips = Texture->GetPlatformData()->Mips;
	if (!Mips.IsValidIndex(MipIndex))
	{
		return;
	}

	FTexture2DMipMap& Mip = Mips[MipIndex];
	const int32 TexelCount = static_cast<int32>(Mip.SizeX) * static_cast<int32>(Mip.SizeY);
	const int64 Bytes = static_cast<int64>(TexelCount) * TextureMipBlend::BytesPerPixel;

	Mip.BulkData.Lock(LOCK_READ_WRITE);
	FColor* Pixels = reinterpret_cast<FColor*>(Mip.BulkData.Realloc(Bytes));
	for (int32 Index = 0; Index < TexelCount; ++Index)
	{
		Pixels[Index] = Color;
	}
	Mip.BulkData.Unlock();
}

void ATextureMipBlendTestActor::CopyMip(UTexture2D* Dest, int32 DestMipIndex, UTexture2D* Source, int32 SourceMipIndex)
{
	if (!Dest || !Source || !Dest->GetPlatformData() || !Source->GetPlatformData())
	{
		return;
	}

	TIndirectArray<FTexture2DMipMap>& DestMips = Dest->GetPlatformData()->Mips;
	TIndirectArray<FTexture2DMipMap>& SourceMips = Source->GetPlatformData()->Mips;
	if (!DestMips.IsValidIndex(DestMipIndex) || !SourceMips.IsValidIndex(SourceMipIndex))
	{
		UE_LOG(LogTextureMipBlend, Error, TEXT("CopyMip out of range dest=%d src=%d"), DestMipIndex, SourceMipIndex);
		return;
	}

	FTexture2DMipMap& DestMip = DestMips[DestMipIndex];
	FTexture2DMipMap& SourceMip = SourceMips[SourceMipIndex];
	if (DestMip.SizeX != SourceMip.SizeX || DestMip.SizeY != SourceMip.SizeY)
	{
		UE_LOG(LogTextureMipBlend, Error, TEXT("CopyMip size mismatch dest=%dx%d src=%dx%d"),
			DestMip.SizeX, DestMip.SizeY, SourceMip.SizeX, SourceMip.SizeY);
		return;
	}

	const int64 Bytes = static_cast<int64>(DestMip.SizeX) * DestMip.SizeY * TextureMipBlend::BytesPerPixel;
	const void* SourceData = SourceMip.BulkData.LockReadOnly();
	DestMip.BulkData.Lock(LOCK_READ_WRITE);
	void* DestData = DestMip.BulkData.Realloc(Bytes);
	FMemory::Memcpy(DestData, SourceData, Bytes);
	DestMip.BulkData.Unlock();
	SourceMip.BulkData.Unlock();
}

void ATextureMipBlendTestActor::EnsureGeneratedTextures(bool bForceRebuild)
{
	const int32 ClampedLoRes = FMath::Clamp(static_cast<int32>(FMath::RoundUpToPowerOfTwo(LoResSize)), 16, 4096);
	if (LoResSize != ClampedLoRes)
	{
		UE_LOG(LogTextureMipBlend, Warning, TEXT("LoResSize %d is not a legal POT in [16,4096]; using %d"), LoResSize, ClampedLoRes);
		LoResSize = ClampedLoRes;
	}

	const int32 HiResSize = LoResSize * 2;

	if (!bForceRebuild
		&& LoResTexture && HiResTexture
		&& LoResTexture->GetSizeX() == LoResSize
		&& HiResTexture->GetSizeX() == HiResSize
		&& LoResTexture->GetResource() != nullptr
		&& HiResTexture->GetResource() != nullptr)
	{
		return;
	}

	LoResTexture = nullptr;
	HiResTexture = nullptr;

	HiResTexture = CreateSolidMipTexture(HiResSize, TEXT("HiResTileTexture"));
	LoResTexture = CreateSolidMipTexture(LoResSize, TEXT("LoResTileTexture"));

	const int32 HiResMipCount = CountMips(HiResSize);
	for (int32 MipIndex = 0; MipIndex < HiResMipCount; ++MipIndex)
	{
		FillMipSolidColor(HiResTexture, MipIndex, GetMipDebugColor(MipIndex));
	}

	const int32 LoResMipCount = CountMips(LoResSize);
	for (int32 MipIndex = 0; MipIndex < LoResMipCount; ++MipIndex)
	{
		CopyMip(LoResTexture, MipIndex, HiResTexture, MipIndex + 1);
	}

	HiResTexture->UpdateResource();
	LoResTexture->UpdateResource();
	FlushRenderingCommands();

	RefreshMipColorDebugInfo();

	UpgradeRefineBias = GetUpgradeRefineBias();

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("Built honest texture pair: lo-res %d (%d mips, finest=%s) is an exact copy of hi-res %d mips [1..%d] (hi-res finest=%s)."),
		LoResSize,
		LoResMipCount,
		*GetMipDebugColor(1).ToString(),
		HiResSize,
		HiResMipCount - 1,
		*GetMipDebugColor(0).ToString());
}

void ATextureMipBlendTestActor::EnsureTextureResourceReady(UTexture2D* Texture)
{
	if (!Texture)
	{
		return;
	}

	if (!Texture->GetResource())
	{
		Texture->UpdateResource();
		FlushRenderingCommands();
	}
}

void ATextureMipBlendTestActor::LogMaterialTextureParameterNames() const
{
	if (!TileMaterial)
	{
		return;
	}

	TArray<FMaterialParameterInfo> ParameterInfos;
	TArray<FGuid> ParameterIds;
	TileMaterial->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);

	if (ParameterInfos.IsEmpty())
	{
		UE_LOG(LogTextureMipBlend, Error,
			TEXT("%s: material '%s' exposes no texture parameters. TileTexture must be a Texture Sample converted to a parameter."),
			*GetName(),
			*TileMaterial->GetName());
		return;
	}

	for (const FMaterialParameterInfo& Info : ParameterInfos)
	{
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: material '%s' texture parameter '%s'"),
			*GetName(),
			*TileMaterial->GetName(),
			*Info.Name.ToString());
	}
}

void ATextureMipBlendTestActor::LogMaterialScalarParameterNames() const
{
	if (!TileMaterial)
	{
		return;
	}

	TArray<FMaterialParameterInfo> ParameterInfos;
	TArray<FGuid> ParameterIds;
	TileMaterial->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);

	for (const FMaterialParameterInfo& Info : ParameterInfos)
	{
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: material '%s' scalar parameter '%s'"),
			*GetName(),
			*TileMaterial->GetName(),
			*Info.Name.ToString());
	}
}

void ATextureMipBlendTestActor::BindTextureToMaterial(UTexture2D* Texture, bool bIsHiResBinding, bool bForceRecreateMid, float RefineBias, float RefineMinMip)
{
	if (!TileMesh || !Texture)
	{
		return;
	}

	if (!TileMaterial)
	{
		UE_LOG(LogTextureMipBlend, Warning,
			TEXT("%s has no TileMaterial. Create /Game/Materials/M_TileMipBlend and assign it on the actor."),
			*GetName());
		return;
	}

	EnsureTextureResourceReady(Texture);

	if (!Texture->GetResource())
	{
		UE_LOG(LogTextureMipBlend, Error,
			TEXT("%s: texture '%s' still has no GPU resource after UpdateResource."),
			*GetName(),
			*Texture->GetName());
		return;
	}

	if (bForceRecreateMid)
	{
		TileMid = nullptr;
	}

	if (!TileMid)
	{
		TileMid = UMaterialInstanceDynamic::Create(TileMaterial, this, NAME_None);
		if (!TileMid)
		{
			UE_LOG(LogTextureMipBlend, Error, TEXT("%s failed to create a dynamic material instance."), *GetName());
			return;
		}
	}

	// Texture + refine scalars in one bind (atomic from the material's point of view).
	const FMaterialParameterInfo TextureParamInfo(TextureMipBlend::TextureParamName);
	const FMaterialParameterInfo BiasParamInfo(TextureMipBlend::RefineBiasParamName);
	const FMaterialParameterInfo MinMipParamInfo(TextureMipBlend::RefineMinMipParamName);

	TileMid->SetTextureParameterValueByInfo(TextureParamInfo, Texture);
	TileMid->SetScalarParameterValueByInfo(BiasParamInfo, RefineBias);
	TileMid->SetScalarParameterValueByInfo(MinMipParamInfo, RefineMinMip);

	// Apply MID to mesh after parameters are set (order matters for some render paths).
	TileMesh->SetMaterial(0, TileMid);

	UTexture* BoundTexture = TileMid->K2_GetTextureParameterValue(TextureMipBlend::TextureParamName);
	if (BoundTexture != Texture)
	{
		LogMaterialTextureParameterNames();
		UE_LOG(LogTextureMipBlend, Error,
			TEXT("%s: MID '%s' TileTexture is '%s', expected '%s'. Parameter name must be exactly '%s'."),
			*GetName(),
			*TileMid->GetName(),
			BoundTexture ? *BoundTexture->GetName() : TEXT("null"),
			*Texture->GetName(),
			*TextureMipBlend::TextureParamName.ToString());
	}
	else
	{
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: bound '%s' on MID '%s' (RefineMinMip=%.2f, RefineBias=%.2f)."),
			*GetName(),
			*Texture->GetName(),
			*TileMid->GetName(),
			RefineMinMip,
			RefineBias);
	}

	bBoundToHiRes = bIsHiResBinding;
	CurrentRefineBias = RefineBias;
	CurrentRefineMinMip = RefineMinMip;

	const TCHAR* ResLabel = bIsHiResBinding ? TEXT("HiRes") : TEXT("LoRes");
	BoundTextureDescription = FString::Printf(
		TEXT("%s %dx%d (%d mips), RefineMinMip=%.2f, RefineBias=%.2f"),
		ResLabel,
		Texture->GetSizeX(),
		Texture->GetSizeY(),
		Texture->GetNumMips(),
		RefineMinMip,
		RefineBias);

	TileMid->RecacheUniformExpressions(false);
	TileMesh->MarkRenderDynamicDataDirty();
	TileMesh->MarkRenderStateDirty();
}
