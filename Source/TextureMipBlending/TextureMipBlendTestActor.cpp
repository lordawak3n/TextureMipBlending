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
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogTextureMipBlend, Log, All);

namespace TextureMipBlend
{
	static const FName TextureParamName(TEXT("TileTexture"));
	static const FName RefineMinMipParamName(TEXT("RefineMinMip"));
	static constexpr int32 BytesPerPixel = 4;

	static TArray<TWeakObjectPtr<ATextureMipBlendTestActor>> ActiveTestActors;
	static TWeakObjectPtr<ATextureMipBlendTestActor> BindInputLeader;
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
	CurrentRefineMinMip = 0.0f;

	// PIE duplicates can retain editor-world texture pointers or a MID that still samples the material default.
	InitializeTileVisuals(/*bForceTextureRebuild=*/true);
	RegisterWithBindGroup();
	TryAcquireBindInputLeader();
	if (IsBindInputLeader())
	{
		SetupSwapInput();
	}

	if (IConsoleVariable* MaxAnisoCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaxAnisotropy")))
	{
		MaxAnisoCVar->Set(FMath::Clamp(MaxTextureAnisotropy, 1, 16), ECVF_SetByCode);
	}

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("%s ready [%s]: %dx%d lo-res bound. Fade RefineMinMip %.2f -> 0 over %.2fs. Checker=%s (%d texels/cell, aniso=%d). '%s'=lo-res, '%s'=hi-res."),
		*GetName(),
		*SamplingLabel,
		LoResTexture ? LoResTexture->GetSizeX() : 0,
		LoResTexture ? LoResTexture->GetSizeY() : 0,
		UpgradeRefineMinMip,
		FadeDurationSeconds,
		bCheckerOnFinestMips ? TEXT("on") : TEXT("off"),
		CheckerTexelsPerCell,
		MaxTextureAnisotropy,
		*LoResKey.GetDisplayName().ToString(),
		*HiResKey.GetDisplayName().ToString());
}

void ATextureMipBlendTestActor::Tick(float DeltaSeconds)
{
	if (!bRefineFadeActive || !bBoundToHiRes || !TileMid)
	{
		return;
	}

	if (FadeDurationSeconds <= 0.0f)
	{
		ApplyRefineMinMipToMid(0.0f);
		StopRefineMinMipFade();
		UE_LOG(LogTextureMipBlend, Display, TEXT("%s: RefineMinMip fade skipped (FadeDurationSeconds=0)."), *GetName());
		return;
	}

	RefineFadeElapsedSeconds += DeltaSeconds;
	const float Alpha = FMath::Clamp(RefineFadeElapsedSeconds / FadeDurationSeconds, 0.0f, 1.0f);
	const float EasedAlpha = FMath::SmoothStep(0.0f, 1.0f, Alpha);
	const float RefineMinMip = RefineFadeStartMinMip * (1.0f - EasedAlpha);

	ApplyRefineMinMipToMid(RefineMinMip);

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

	const bool bWasInputLeader = IsBindInputLeader();
	UnregisterFromBindGroup();

	if (bWasInputLeader)
	{
		TextureMipBlend::BindInputLeader.Reset();
		for (const TWeakObjectPtr<ATextureMipBlendTestActor>& WeakActor : TextureMipBlend::ActiveTestActors)
		{
			if (ATextureMipBlendTestActor* Actor = WeakActor.Get())
			{
				TextureMipBlend::BindInputLeader = Actor;
				Actor->SetupSwapInput();
				break;
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

void ATextureMipBlendTestActor::RegisterWithBindGroup()
{
	TextureMipBlend::ActiveTestActors.AddUnique(this);
}

void ATextureMipBlendTestActor::UnregisterFromBindGroup()
{
	TextureMipBlend::ActiveTestActors.RemoveAll(
		[this](const TWeakObjectPtr<ATextureMipBlendTestActor>& WeakActor)
		{
			return !WeakActor.IsValid() || WeakActor.Get() == this;
		});
}

bool ATextureMipBlendTestActor::IsBindInputLeader() const
{
	return TextureMipBlend::BindInputLeader.Get() == this;
}

void ATextureMipBlendTestActor::TryAcquireBindInputLeader()
{
	if (!TextureMipBlend::BindInputLeader.IsValid())
	{
		TextureMipBlend::BindInputLeader = this;
	}
}

void ATextureMipBlendTestActor::BroadcastBindRequest(bool bBindHiRes, const FKey& SourceKey)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 SyncedActorCount = 0;
	for (const TWeakObjectPtr<ATextureMipBlendTestActor>& WeakActor : TextureMipBlend::ActiveTestActors)
	{
		ATextureMipBlendTestActor* Actor = WeakActor.Get();
		if (!Actor || Actor->GetWorld() != World || !Actor->bSyncTextureBindsWithGroup)
		{
			continue;
		}

		++SyncedActorCount;
		Actor->RequestTextureBind(bBindHiRes, SourceKey);
	}

	if (SyncedActorCount <= 1)
	{
		return;
	}

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("Bind group: '%s' -> %s on %d synced actors."),
		*SourceKey.GetDisplayName().ToString(),
		bBindHiRes ? TEXT("hi-res") : TEXT("lo-res"),
		SyncedActorCount);
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
	BroadcastBindRequest(/*bBindHiRes=*/false, LoResKey);
}

void ATextureMipBlendTestActor::HandleHiResKeyPressed()
{
	BroadcastBindRequest(/*bBindHiRes=*/true, HiResKey);
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
	const float ExpectedMinMip = GetRefineMinMipForBind(bPendingBindHiRes);

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
	const float RefineMinMip = GetRefineMinMipForBind(bPendingBindHiRes);
	BindTextureToMaterial(TextureToBind, bPendingBindHiRes, /*bForceRecreateMid=*/false, RefineMinMip);

	if (bPendingBindHiRes)
	{
		StartRefineMinMipFade();
		UE_LOG(LogTextureMipBlend, Display,
			TEXT("%s: hi-res bound — RefineMinMip fading %.2f -> 0 over %.2fs."),
			*GetName(),
			RefineFadeStartMinMip,
			FadeDurationSeconds);
	}
	else if (!bWasLoRes)
	{
		UE_LOG(LogTextureMipBlend, Display, TEXT("%s: lo-res bound (RefineMinMip=0)."), *GetName());
	}
}

#if WITH_EDITOR
void ATextureMipBlendTestActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, TileMeshScale))
	{
		ApplyTileTransform();
		return;
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, LoResSize)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, TileMaterial)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, bCheckerOnFinestMips)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, CheckerTexelsPerCell)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, MaxTextureAnisotropy))
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, LoResSize)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, bCheckerOnFinestMips)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, CheckerTexelsPerCell)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(ATextureMipBlendTestActor, MaxTextureAnisotropy))
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
	EnsureGeneratedTextures(bForceTextureRebuild);

	if (LoResTexture)
	{
		BindTextureToMaterial(LoResTexture, /*bIsHiResBinding=*/false, /*bForceRecreateMid=*/true, /*RefineMinMip=*/0.0f);
	}
}

float ATextureMipBlendTestActor::GetRefineMinMipForBind(bool bIsHiResBinding) const
{
	return bIsHiResBinding ? GetUpgradeRefineMinMip() : 0.0f;
}

float ATextureMipBlendTestActor::GetUpgradeRefineMinMip() const
{
	if (LoResSize <= 0)
	{
		return 0.0f;
	}

	const int32 HiResSize = LoResSize * 2;
	return FMath::Log2(static_cast<float>(HiResSize) / static_cast<float>(LoResSize));
}

int32 ATextureMipBlendTestActor::GetCheckerCellsPerEdge() const
{
	const int32 ClampedCellSize = FMath::Clamp(CheckerTexelsPerCell, 2, 256);
	return FMath::Max(LoResSize / ClampedCellSize, 2);
}

int32 ATextureMipBlendTestActor::GetCheckerTexelsPerCellForMipSize(int32 MipSize) const
{
	const int32 CellsPerEdge = GetCheckerCellsPerEdge();
	return FMath::Clamp(MipSize / CellsPerEdge, 2, MipSize);
}

void ATextureMipBlendTestActor::ApplyRefineMinMipToMid(float RefineMinMip)
{
	if (!TileMid || !MaterialSupportsRefineMinMip())
	{
		return;
	}

	const FMaterialParameterInfo MinMipParamInfo(TextureMipBlend::RefineMinMipParamName);
	TileMid->SetScalarParameterValueByInfo(MinMipParamInfo, RefineMinMip);
	CurrentRefineMinMip = RefineMinMip;
}

bool ATextureMipBlendTestActor::MaterialSupportsRefineMinMip() const
{
	if (!TileMaterial)
	{
		return false;
	}

	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarIds;
	TileMaterial->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);

	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		if (Info.Name == TextureMipBlend::RefineMinMipParamName)
		{
			return true;
		}
	}

	return false;
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

	// Engine plane is 100x100 uu (1m). TileMeshScale sets world size on each axis.
	TileMesh->SetRelativeScale3D(TileMeshScale);
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

FString ATextureMipBlendTestActor::GetMipDebugDisplayName(int32 MipIndexFromFinest, bool bChecker)
{
	const FName ColorName = GetMipDebugColorName(MipIndexFromFinest);
	if (bChecker)
	{
		return FString::Printf(TEXT("%s Checker"), *ColorName.ToString());
	}
	return ColorName.ToString();
}

FColor ATextureMipBlendTestActor::GetMipCheckerAlternateColor(FColor BaseColor)
{
	// Dark alternate square — high contrast for grazing-angle / aniso streaks.
	return FColor(
		static_cast<uint8>(BaseColor.R >> 2),
		static_cast<uint8>(BaseColor.G >> 2),
		static_cast<uint8>(BaseColor.B >> 2),
		255);
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
		Entry.ColorName = GetMipDebugDisplayName(MipIndex, bCheckerOnFinestMips);
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
			Entry.ColorName = GetMipDebugDisplayName(SourceHiResMip, bCheckerOnFinestMips);
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

void ATextureMipBlendTestActor::FillMipCheckerPattern(UTexture2D* Texture, int32 MipIndex, FColor BaseColor, int32 TexelsPerCell)
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

	const int32 ClampedCellSize = FMath::Clamp(TexelsPerCell, 2, 256);
	const FColor AlternateColor = GetMipCheckerAlternateColor(BaseColor);

	FTexture2DMipMap& Mip = Mips[MipIndex];
	const int32 Width = static_cast<int32>(Mip.SizeX);
	const int32 Height = static_cast<int32>(Mip.SizeY);
	const int32 TexelCount = Width * Height;
	const int64 Bytes = static_cast<int64>(TexelCount) * TextureMipBlend::BytesPerPixel;

	Mip.BulkData.Lock(LOCK_READ_WRITE);
	FColor* Pixels = reinterpret_cast<FColor*>(Mip.BulkData.Realloc(Bytes));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 CellY = Y / ClampedCellSize;
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 CellX = X / ClampedCellSize;
			const bool bAlternate = ((CellX + CellY) & 1) != 0;
			Pixels[Y * Width + X] = bAlternate ? AlternateColor : BaseColor;
		}
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
	const int32 CellsPerEdge = GetCheckerCellsPerEdge();
	for (int32 MipIndex = 0; MipIndex < HiResMipCount; ++MipIndex)
	{
		const FColor MipColor = GetMipDebugColor(MipIndex);
		const int32 MipSize = HiResSize >> MipIndex;
		if (bCheckerOnFinestMips)
		{
			FillMipCheckerPattern(HiResTexture, MipIndex, MipColor, GetCheckerTexelsPerCellForMipSize(MipSize));
		}
		else
		{
			FillMipSolidColor(HiResTexture, MipIndex, MipColor);
		}
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

	UpgradeRefineMinMip = GetUpgradeRefineMinMip();

	UE_LOG(LogTextureMipBlend, Display,
		TEXT("Built honest texture pair [%s]: lo-res %d (%d mips) copies hi-res %d mips [1..%d]. Checker=%s (%d cells/edge, lo-res finest=%d texels/cell, hi-res mip0=%d texels/cell)."),
		*SamplingLabel,
		LoResSize,
		LoResMipCount,
		HiResSize,
		HiResMipCount - 1,
		bCheckerOnFinestMips ? TEXT("all mips") : TEXT("off"),
		CellsPerEdge,
		GetCheckerTexelsPerCellForMipSize(LoResSize),
		GetCheckerTexelsPerCellForMipSize(HiResSize));
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

void ATextureMipBlendTestActor::BindTextureToMaterial(UTexture2D* Texture, bool bIsHiResBinding, bool bForceRecreateMid, float RefineMinMip)
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

	// Texture + RefineMinMip in one bind (atomic from the material's point of view).
	const FMaterialParameterInfo TextureParamInfo(TextureMipBlend::TextureParamName);
	const FMaterialParameterInfo MinMipParamInfo(TextureMipBlend::RefineMinMipParamName);

	TileMid->SetTextureParameterValueByInfo(TextureParamInfo, Texture);

	const bool bSupportsRefineMinMip = MaterialSupportsRefineMinMip();
	if (bSupportsRefineMinMip)
	{
		TileMid->SetScalarParameterValueByInfo(MinMipParamInfo, RefineMinMip);
		CurrentRefineMinMip = RefineMinMip;
	}
	else
	{
		CurrentRefineMinMip = 0.0f;
	}

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
		if (bSupportsRefineMinMip)
		{
			UE_LOG(LogTextureMipBlend, Display,
				TEXT("%s [%s]: bound '%s' on MID '%s' (RefineMinMip=%.2f)."),
				*GetName(),
				*SamplingLabel,
				*Texture->GetName(),
				*TileMid->GetName(),
				RefineMinMip);
		}
		else
		{
			UE_LOG(LogTextureMipBlend, Display,
				TEXT("%s [%s]: bound '%s' on MID '%s' (default sampler — no RefineMinMip)."),
				*GetName(),
				*SamplingLabel,
				*Texture->GetName(),
				*TileMid->GetName());
		}
	}

	bBoundToHiRes = bIsHiResBinding;

	const TCHAR* ResLabel = bIsHiResBinding ? TEXT("HiRes") : TEXT("LoRes");
	BoundTextureDescription = FString::Printf(
		TEXT("%s %dx%d (%d mips)%s"),
		ResLabel,
		Texture->GetSizeX(),
		Texture->GetSizeY(),
		Texture->GetNumMips(),
		bSupportsRefineMinMip
			? *FString::Printf(TEXT(", RefineMinMip=%.2f"), RefineMinMip)
			: TEXT(", default sampler"));

	TileMid->RecacheUniformExpressions(false);
	TileMesh->MarkRenderDynamicDataDirty();
	TileMesh->MarkRenderStateDirty();
}
