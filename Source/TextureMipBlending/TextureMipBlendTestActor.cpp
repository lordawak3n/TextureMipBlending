#include "TextureMipBlendTestActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogTextureMipBlend, Log, All);

namespace TextureMipBlend
{
	static const FName TextureParamName(TEXT("TileTexture"));
	static const FName RefineBiasParamName(TEXT("RefineBias"));
	static constexpr int32 BytesPerPixel = 4;
}

ATextureMipBlendTestActor::ATextureMipBlendTestActor()
{
	PrimaryActorTick.bCanEverTick = false;

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
}

void ATextureMipBlendTestActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Editor-only preview (BP editor / placed actor viewport). PIE/game always re-inits in BeginPlay.
	if (GetWorld() && !GetWorld()->IsGameWorld())
	{
		InitializeTileVisuals(/*bForceTextureRebuild=*/false);
	}
	else
	{
		ApplyTileTransform();
	}
}

void ATextureMipBlendTestActor::BeginPlay()
{
	Super::BeginPlay();

	// PIE duplicates can retain editor-world texture pointers or a MID that still samples the material default.
	InitializeTileVisuals(/*bForceTextureRebuild=*/true);

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("%s Phase 1 ready: bound %dx%d lo-res (%d mips) on MID '%s'. Hi-res %dx%d generated but not bound. TileTexture param = '%s'."),
		*GetName(),
		LoResTexture ? LoResTexture->GetSizeX() : 0,
		LoResTexture ? LoResTexture->GetSizeY() : 0,
		LoResTexture ? LoResTexture->GetNumMips() : 0,
		TileMid ? *TileMid->GetName() : TEXT("null"),
		HiResTexture ? HiResTexture->GetSizeX() : 0,
		HiResTexture ? HiResTexture->GetSizeY() : 0,
		*TextureMipBlend::TextureParamName.ToString());
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
	BindLoResToMaterial(bForceTextureRebuild);
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

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		FlushRenderingCommands();
	}

	RefreshMipColorDebugInfo();

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("Built honest texture pair: lo-res %d (%d mips, finest=%s) is an exact copy of hi-res %d mips [1..%d] (hi-res finest=%s)."),
		LoResSize,
		LoResMipCount,
		*GetMipDebugColor(1).ToString(),
		HiResSize,
		HiResMipCount - 1,
		*GetMipDebugColor(0).ToString());
}

void ATextureMipBlendTestActor::BindLoResToMaterial(bool bForceRecreateMid)
{
	if (!TileMesh)
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

	if (!LoResTexture || !LoResTexture->GetResource())
	{
		UE_LOG(LogTextureMipBlend, Warning, TEXT("%s has no valid lo-res texture resource to bind."), *GetName());
		return;
	}

	if (bForceRecreateMid || !TileMid || TileMid->Parent != TileMaterial)
	{
		TileMid = UMaterialInstanceDynamic::Create(TileMaterial, this, NAME_None);
		if (!TileMid)
		{
			UE_LOG(LogTextureMipBlend, Error, TEXT("%s failed to create a dynamic material instance."), *GetName());
			return;
		}

		TileMesh->SetMaterial(0, TileMid);
	}

	TileMid->SetTextureParameterValue(TextureMipBlend::TextureParamName, LoResTexture);
	TileMid->SetScalarParameterValue(TextureMipBlend::RefineBiasParamName, 0.0f);

	if (TileMid->K2_GetTextureParameterValue(TextureMipBlend::TextureParamName) != LoResTexture)
	{
		UE_LOG(LogTextureMipBlend, Error,
			TEXT("%s: MID '%s' did not retain texture parameter '%s'. Check M_TileMipBlend parameter name matches exactly."),
			*GetName(),
			*TileMid->GetName(),
			*TextureMipBlend::TextureParamName.ToString());
	}

	BoundTextureDescription = FString::Printf(
		TEXT("LoRes %dx%d (%d mips), RefineBias=0"),
		LoResTexture->GetSizeX(),
		LoResTexture->GetSizeY(),
		LoResTexture->GetNumMips());

	TileMid->RecacheUniformExpressions(false);
	TileMesh->MarkRenderDynamicDataDirty();
	TileMesh->MarkRenderStateDirty();
}
