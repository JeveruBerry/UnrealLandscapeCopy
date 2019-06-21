// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
CyLandRender.cpp: New terrain rendering
=============================================================================*/

#include "CyLandRender.h"
#include "LightMap.h"
#include "ShadowMap.h"
#include "CyLandLayerInfoObject.h"
#include "CyLandPrivate.h"
#include "CyLandMeshProxyComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionCyLandLayerCoords.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ShaderParameterUtils.h"
#include "TessellationRendering.h"
#include "CyLandEdit.h"
#include "Engine/LevelStreaming.h"
#include "LevelUtils.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "CyLandMaterialInstanceConstant.h"
#include "Engine/ShadowMapTexture2D.h"
#include "EngineGlobals.h"
#include "UnrealEngine.h"
#include "CyLandLight.h"
#include "Algo/Find.h"
#include "Engine/StaticMesh.h"
#include "CyLandInfo.h"
#include "CyLandDataAccess.h"
#include "DrawDebugHelpers.h"
#include "PrimitiveSceneInfo.h"
#include "SceneView.h"
#include "Runtime/Renderer/Private/SceneCore.h"
#include "CyLandProxy.h"
#include "MeshMaterialShader.h"

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FCyLandCyUniformShaderParameters, "LandscapeParameters"); //"CyLandParameters"

int32 GCyLandMeshLODBias = 0;
FAutoConsoleVariableRef CVarCyLandMeshLODBias(
	TEXT("r.CyLandLODBias"),
	GCyLandMeshLODBias,
	TEXT("LOD bias for landscape/terrain meshes."),
	ECVF_Scalability
);

float GCyLandLOD0DistributionScale = 1.f;
FAutoConsoleVariableRef CVarCyLandLOD0DistributionScale(
	TEXT("r.CyLandLOD0DistributionScale"),
	GCyLandLOD0DistributionScale,
	TEXT("Multiplier for the landscape LOD0DistributionSetting property"),
	ECVF_Scalability
);

float GCyLandLODDistributionScale = 1.f;
FAutoConsoleVariableRef CVarCyLandLODDistributionScale(
	TEXT("r.CyLandLODDistributionScale"),
	GCyLandLODDistributionScale,
	TEXT("Multiplier for the landscape LODDistributionSetting property"),
	ECVF_Scalability
);

float GShadowMapWorldUnitsToTexelFactor = -1.0f;
static FAutoConsoleVariableRef CVarShadowMapWorldUnitsToTexelFactor(
	TEXT("CyLand.ShadowMapWorldUnitsToTexelFactor"),
	GShadowMapWorldUnitsToTexelFactor,
	TEXT("Used to specify tolerance factor for mesh size related to cascade shadow resolution")
);

int32 GAllowCyLandShadows = 1;
static FAutoConsoleVariableRef CVarAllowCyLandShadows(
	TEXT("r.AllowCyLandShadows"),
	GAllowCyLandShadows,
	TEXT("Allow CyLand Shadows")
);

#if !UE_BUILD_SHIPPING
static void OnLODDistributionScaleChanged(IConsoleVariable* CVar)
{
	for (auto* CyLandComponent : TObjectRange<UCyLandComponent>(RF_ClassDefaultObject | RF_ArchetypeObject, true, EInternalObjectFlags::PendingKill))
	{
		CyLandComponent->MarkRenderStateDirty();
	}
}

int32 GVarDumpCyLandLODsCurrentFrame = 0;
bool GVarDumpCyLandLODs = false;

static void OnDumpCyLandLODs(const TArray< FString >& Args)
{
	if (Args.Num() >= 1)
	{
		GVarDumpCyLandLODs = FCString::Atoi(*Args[0]) == 0 ? false : true;
	}

	// Add some buffer to be able to correctly catch the frame during the rendering
	GVarDumpCyLandLODsCurrentFrame = GVarDumpCyLandLODs ? GFrameNumberRenderThread + 3 : INDEX_NONE;
}

static FAutoConsoleCommand CVarDumpCyLandLODs(
	TEXT("CyLand.DumpLODs"),
	TEXT("Will dump the current status of LOD value and current texture streaming status"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&OnDumpCyLandLODs)
);
#endif

#if WITH_EDITOR
CYLAND_API int32 GCyLandViewMode = ECyLandViewMode::Normal;
FAutoConsoleVariableRef CVarCyLandDebugViewMode(
	TEXT("CyLand.DebugViewMode"),
	GCyLandViewMode,
	TEXT("Change the view mode of the landscape rendering. Valid Input: 0 = Normal, 2 = DebugLayer, 3 = LayerDensity, 4 = LayerUsage, 5 = LOD Distribution, 6 = WireframeOnTop"),
	ECVF_Cheat
);
#endif

/*------------------------------------------------------------------------------
Forsyth algorithm for cache optimizing index buffers.
------------------------------------------------------------------------------*/

// Forsyth algorithm to optimize post-transformed vertex cache
namespace
{
	// code for computing vertex score was taken, as much as possible
	// directly from the original publication.
	float ComputeVertexCacheScore(int32 CachePosition, uint32 VertexCacheSize)
	{
		const float FindVertexScoreCacheDecayPower = 1.5f;
		const float FindVertexScoreLastTriScore = 0.75f;

		float Score = 0.0f;
		if (CachePosition < 0)
		{
			// Vertex is not in FIFO cache - no score.
		}
		else
		{
			if (CachePosition < 3)
			{
				// This vertex was used in the last triangle,
				// so it has a fixed score, whichever of the three
				// it's in. Otherwise, you can get very different
				// answers depending on whether you add
				// the triangle 1,2,3 or 3,1,2 - which is silly.
				Score = FindVertexScoreLastTriScore;
			}
			else
			{
				check(CachePosition < (int32)VertexCacheSize);
				// Points for being high in the cache.
				const float Scaler = 1.0f / (VertexCacheSize - 3);
				Score = 1.0f - (CachePosition - 3) * Scaler;
				Score = FMath::Pow(Score, FindVertexScoreCacheDecayPower);
			}
		}

		return Score;
	}

	float ComputeVertexValenceScore(uint32 numActiveFaces)
	{
		const float FindVertexScoreValenceBoostScale = 2.0f;
		const float FindVertexScoreValenceBoostPower = 0.5f;

		float Score = 0.f;

		// Bonus points for having a low number of tris still to
		// use the vert, so we get rid of lone verts quickly.
		float ValenceBoost = FMath::Pow(float(numActiveFaces), -FindVertexScoreValenceBoostPower);
		Score += FindVertexScoreValenceBoostScale * ValenceBoost;

		return Score;
	}

	const uint32 MaxVertexCacheSize = 64;
	const uint32 MaxPrecomputedVertexValenceScores = 64;
	float VertexCacheScores[MaxVertexCacheSize + 1][MaxVertexCacheSize];
	float VertexValenceScores[MaxPrecomputedVertexValenceScores];
	bool bVertexScoresComputed = false; //ComputeVertexScores();

	bool ComputeVertexScores()
	{
		for (uint32 CacheSize = 0; CacheSize <= MaxVertexCacheSize; ++CacheSize)
		{
			for (uint32 CachePos = 0; CachePos < CacheSize; ++CachePos)
			{
				VertexCacheScores[CacheSize][CachePos] = ComputeVertexCacheScore(CachePos, CacheSize);
			}
		}

		for (uint32 Valence = 0; Valence < MaxPrecomputedVertexValenceScores; ++Valence)
		{
			VertexValenceScores[Valence] = ComputeVertexValenceScore(Valence);
		}

		return true;
	}

	inline float FindVertexCacheScore(uint32 CachePosition, uint32 MaxSizeVertexCache)
	{
		return VertexCacheScores[MaxSizeVertexCache][CachePosition];
	}

	inline float FindVertexValenceScore(uint32 NumActiveTris)
	{
		return VertexValenceScores[NumActiveTris];
	}

	float FindVertexScore(uint32 NumActiveFaces, uint32 CachePosition, uint32 VertexCacheSize)
	{
		check(bVertexScoresComputed);

		if (NumActiveFaces == 0)
		{
			// No tri needs this vertex!
			return -1.0f;
		}

		float Score = 0.f;
		if (CachePosition < VertexCacheSize)
		{
			Score += VertexCacheScores[VertexCacheSize][CachePosition];
		}

		if (NumActiveFaces < MaxPrecomputedVertexValenceScores)
		{
			Score += VertexValenceScores[NumActiveFaces];
		}
		else
		{
			Score += ComputeVertexValenceScore(NumActiveFaces);
		}

		return Score;
	}

	struct OptimizeVertexData
	{
		float  Score;
		uint32  ActiveFaceListStart;
		uint32  ActiveFaceListSize;
		uint32  CachePos0;
		uint32  CachePos1;
		OptimizeVertexData() : Score(0.f), ActiveFaceListStart(0), ActiveFaceListSize(0), CachePos0(0), CachePos1(0) { }
	};

	//-----------------------------------------------------------------------------
	//  OptimizeFaces
	//-----------------------------------------------------------------------------
	//  Parameters:
	//      InIndexList
	//          input index list
	//      OutIndexList
	//          a pointer to a preallocated buffer the same size as indexList to
	//          hold the optimized index list
	//      LRUCacheSize
	//          the size of the simulated post-transform cache (max:64)
	//-----------------------------------------------------------------------------

	template <typename INDEX_TYPE>
	void OptimizeFaces(const TArray<INDEX_TYPE>& InIndexList, TArray<INDEX_TYPE>& OutIndexList, uint16 LRUCacheSize)
	{
		uint32 VertexCount = 0;
		const uint32 IndexCount = InIndexList.Num();

		// compute face count per vertex
		for (uint32 i = 0; i < IndexCount; ++i)
		{
			uint32 Index = InIndexList[i];
			VertexCount = FMath::Max(Index, VertexCount);
		}
		VertexCount++;

		TArray<OptimizeVertexData> VertexDataList;
		VertexDataList.Empty(VertexCount);
		for (uint32 i = 0; i < VertexCount; i++)
		{
			VertexDataList.Add(OptimizeVertexData());
		}

		OutIndexList.Empty(IndexCount);
		OutIndexList.AddZeroed(IndexCount);

		// compute face count per vertex
		for (uint32 i = 0; i < IndexCount; ++i)
		{
			uint32 Index = InIndexList[i];
			OptimizeVertexData& VertexData = VertexDataList[Index];
			VertexData.ActiveFaceListSize++;
		}

		TArray<uint32> ActiveFaceList;

		const uint32 EvictedCacheIndex = TNumericLimits<uint32>::Max();

		{
			// allocate face list per vertex
			uint32 CurActiveFaceListPos = 0;
			for (uint32 i = 0; i < VertexCount; ++i)
			{
				OptimizeVertexData& VertexData = VertexDataList[i];
				VertexData.CachePos0 = EvictedCacheIndex;
				VertexData.CachePos1 = EvictedCacheIndex;
				VertexData.ActiveFaceListStart = CurActiveFaceListPos;
				CurActiveFaceListPos += VertexData.ActiveFaceListSize;
				VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos0, LRUCacheSize);
				VertexData.ActiveFaceListSize = 0;
			}
			ActiveFaceList.Empty(CurActiveFaceListPos);
			ActiveFaceList.AddZeroed(CurActiveFaceListPos);
		}

		// fill out face list per vertex
		for (uint32 i = 0; i < IndexCount; i += 3)
		{
			for (uint32 j = 0; j < 3; ++j)
			{
				uint32 Index = InIndexList[i + j];
				OptimizeVertexData& VertexData = VertexDataList[Index];
				ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize] = i;
				VertexData.ActiveFaceListSize++;
			}
		}

		TArray<uint8> ProcessedFaceList;
		ProcessedFaceList.Empty(IndexCount);
		ProcessedFaceList.AddZeroed(IndexCount);

		uint32 VertexCacheBuffer[(MaxVertexCacheSize + 3) * 2];
		uint32* Cache0 = VertexCacheBuffer;
		uint32* Cache1 = VertexCacheBuffer + (MaxVertexCacheSize + 3);
		uint32 EntriesInCache0 = 0;

		uint32 BestFace = 0;
		float BestScore = -1.f;

		const float MaxValenceScore = FindVertexScore(1, EvictedCacheIndex, LRUCacheSize) * 3.f;

		for (uint32 i = 0; i < IndexCount; i += 3)
		{
			if (BestScore < 0.f)
			{
				// no verts in the cache are used by any unprocessed faces so
				// search all unprocessed faces for a new starting point
				for (uint32 j = 0; j < IndexCount; j += 3)
				{
					if (ProcessedFaceList[j] == 0)
					{
						uint32 Face = j;
						float FaceScore = 0.f;
						for (uint32 k = 0; k < 3; ++k)
						{
							uint32 Index = InIndexList[Face + k];
							OptimizeVertexData& VertexData = VertexDataList[Index];
							check(VertexData.ActiveFaceListSize > 0);
							check(VertexData.CachePos0 >= LRUCacheSize);
							FaceScore += VertexData.Score;
						}

						if (FaceScore > BestScore)
						{
							BestScore = FaceScore;
							BestFace = Face;

							check(BestScore <= MaxValenceScore);
							if (BestScore >= MaxValenceScore)
							{
								break;
							}
						}
					}
				}
				check(BestScore >= 0.f);
			}

			ProcessedFaceList[BestFace] = 1;
			uint32 EntriesInCache1 = 0;

			// add bestFace to LRU cache and to newIndexList
			for (uint32 V = 0; V < 3; ++V)
			{
				INDEX_TYPE Index = InIndexList[BestFace + V];
				OutIndexList[i + V] = Index;

				OptimizeVertexData& VertexData = VertexDataList[Index];

				if (VertexData.CachePos1 >= EntriesInCache1)
				{
					VertexData.CachePos1 = EntriesInCache1;
					Cache1[EntriesInCache1++] = Index;

					if (VertexData.ActiveFaceListSize == 1)
					{
						--VertexData.ActiveFaceListSize;
						continue;
					}
				}

				check(VertexData.ActiveFaceListSize > 0);
				uint32 FindIndex;
				for (FindIndex = VertexData.ActiveFaceListStart; FindIndex < VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize; FindIndex++)
				{
					if (ActiveFaceList[FindIndex] == BestFace)
					{
						break;
					}
				}
				check(FindIndex != VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize);

				if (FindIndex != VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1)
				{
					uint32 SwapTemp = ActiveFaceList[FindIndex];
					ActiveFaceList[FindIndex] = ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1];
					ActiveFaceList[VertexData.ActiveFaceListStart + VertexData.ActiveFaceListSize - 1] = SwapTemp;
				}

				--VertexData.ActiveFaceListSize;
				VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos1, LRUCacheSize);

			}

			// move the rest of the old verts in the cache down and compute their new scores
			for (uint32 C0 = 0; C0 < EntriesInCache0; ++C0)
			{
				uint32 Index = Cache0[C0];
				OptimizeVertexData& VertexData = VertexDataList[Index];

				if (VertexData.CachePos1 >= EntriesInCache1)
				{
					VertexData.CachePos1 = EntriesInCache1;
					Cache1[EntriesInCache1++] = Index;
					VertexData.Score = FindVertexScore(VertexData.ActiveFaceListSize, VertexData.CachePos1, LRUCacheSize);
				}
			}

			// find the best scoring triangle in the current cache (including up to 3 that were just evicted)
			BestScore = -1.f;
			for (uint32 C1 = 0; C1 < EntriesInCache1; ++C1)
			{
				uint32 Index = Cache1[C1];
				OptimizeVertexData& VertexData = VertexDataList[Index];
				VertexData.CachePos0 = VertexData.CachePos1;
				VertexData.CachePos1 = EvictedCacheIndex;
				for (uint32 j = 0; j < VertexData.ActiveFaceListSize; ++j)
				{
					uint32 Face = ActiveFaceList[VertexData.ActiveFaceListStart + j];
					float FaceScore = 0.f;
					for (uint32 V = 0; V < 3; V++)
					{
						uint32 FaceIndex = InIndexList[Face + V];
						OptimizeVertexData& FaceVertexData = VertexDataList[FaceIndex];
						FaceScore += FaceVertexData.Score;
					}
					if (FaceScore > BestScore)
					{
						BestScore = FaceScore;
						BestFace = Face;
					}
				}
			}

			uint32* SwapTemp = Cache0;
			Cache0 = Cache1;
			Cache1 = SwapTemp;

			EntriesInCache0 = FMath::Min(EntriesInCache1, (uint32)LRUCacheSize);
		}
	}

} // namespace 

struct FCyLandDebugOptions
{
	FCyLandDebugOptions()
		: bShowPatches(false)
		, bDisableStatic(false)
		, CombineMode(eCombineMode_Default)
		, PatchesConsoleCommand(
			TEXT("CyLand.Patches"),
			TEXT("Show/hide CyLand patches"),
			FConsoleCommandDelegate::CreateRaw(this, &FCyLandDebugOptions::Patches))
		, StaticConsoleCommand(
			TEXT("CyLand.Static"),
			TEXT("Enable/disable CyLand static drawlists"),
			FConsoleCommandDelegate::CreateRaw(this, &FCyLandDebugOptions::Static))
		, CombineConsoleCommand(
			TEXT("CyLand.Combine"),
			TEXT("Set landscape component combining mode : 0 = Default, 1 = Combine All, 2 = Disabled"),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FCyLandDebugOptions::Combine))
	{
	}

	enum eCombineMode
	{
		eCombineMode_Default = 0,
		eCombineMode_CombineAll = 1,
		eCombineMode_Disabled = 2
	};

	bool bShowPatches;
	bool bDisableStatic;
	eCombineMode CombineMode;

	FORCEINLINE bool IsCombinedDisabled() const { return CombineMode == eCombineMode_Disabled; }
	FORCEINLINE bool IsCombinedAll() const { return CombineMode == eCombineMode_CombineAll; }
	FORCEINLINE bool IsCombinedDefault() const { return CombineMode == eCombineMode_Default; }

private:
	FAutoConsoleCommand PatchesConsoleCommand;
	FAutoConsoleCommand StaticConsoleCommand;
	FAutoConsoleCommand CombineConsoleCommand;

	void Patches()
	{
		bShowPatches = !bShowPatches;
		UE_LOG(LogCyLand, Display, TEXT("CyLand.Patches: %s"), bShowPatches ? TEXT("Show") : TEXT("Hide"));
	}

	void Static()
	{
		bDisableStatic = !bDisableStatic;
		UE_LOG(LogCyLand, Display, TEXT("CyLand.Static: %s"), bDisableStatic ? TEXT("Disabled") : TEXT("Enabled"));
	}

	void Combine(const TArray<FString>& Args)
	{
		if (Args.Num() >= 1)
		{
			CombineMode = (eCombineMode)FCString::Atoi(*Args[0]);
			UE_LOG(LogCyLand, Display, TEXT("CyLand.Combine: %d"), (int32)CombineMode);
		}
	}
};

FCyLandDebugOptions GCyLandDebugOptions;


#if WITH_EDITOR
CYLAND_API bool GCyLandEditModeActive = false;
CYLAND_API int32 GCyLandEditRenderMode = ECyLandEditRenderMode::None;
UMaterialInterface* GLayerDebugColorMaterial = nullptr;
UMaterialInterface* GSelectionColorMaterial = nullptr;
UMaterialInterface* GSelectionRegionMaterial = nullptr;
UMaterialInterface* GMaskRegionMaterial = nullptr;
UTexture2D* GCyLandBlackTexture = nullptr;
UMaterialInterface* GCyLandLayerUsageMaterial = nullptr;
#endif

void UCyLandComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// TODO - investigate whether this is correct

	ACyLandProxy* Actor = GetCyLandProxy();

	if (Actor != nullptr && Actor->bUseDynamicMaterialInstance)
	{
		OutMaterials.Append(MaterialInstancesDynamic.FilterByPredicate([](UMaterialInstanceDynamic* MaterialInstance) { return MaterialInstance != nullptr; }));
	}
	else
	{
		OutMaterials.Append(MaterialInstances.FilterByPredicate([](UMaterialInstanceConstant* MaterialInstance) { return MaterialInstance != nullptr; }));
	}

	if (OverrideMaterial)
	{
		OutMaterials.Add(OverrideMaterial);
	}

	if (OverrideHoleMaterial)
	{
		OutMaterials.Add(OverrideHoleMaterial);
	}

	OutMaterials.Append(MobileMaterialInterfaces);

#if WITH_EDITORONLY_DATA
	if (EditToolRenderData.ToolMaterial)
	{
		OutMaterials.Add(EditToolRenderData.ToolMaterial);
	}

	if (EditToolRenderData.GizmoMaterial)
	{
		OutMaterials.Add(EditToolRenderData.GizmoMaterial);
	}
#endif

#if WITH_EDITOR
	//if (bGetDebugMaterials) // TODO: This should be tested and enabled
	{
		OutMaterials.Add(GLayerDebugColorMaterial);
		OutMaterials.Add(GSelectionColorMaterial);
		OutMaterials.Add(GSelectionRegionMaterial);
		OutMaterials.Add(GMaskRegionMaterial);
		OutMaterials.Add(GCyLandLayerUsageMaterial);
	}
#endif
}

//
// FCyLandComponentSceneProxy
//
TMap<uint32, FCyLandSharedBuffers*>FCyLandComponentSceneProxy::SharedBuffersMap;
TMap<FCyLandNeighborInfo::FCyLandKey, TMap<FIntPoint, const FCyLandNeighborInfo*> > FCyLandNeighborInfo::SharedSceneProxyMap;

const static FName NAME_CyLandResourceNameForDebugging(TEXT("CyLand"));

FCyLandComponentSceneProxy::FCyLandComponentSceneProxy(UCyLandComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent, NAME_CyLandResourceNameForDebugging)
	, FCyLandNeighborInfo(InComponent->GetWorld(), InComponent->GetCyLandProxy()->GetCyLandGuid(), InComponent->GetSectionBase() / InComponent->ComponentSizeQuads, InComponent->GetHeightmap(), InComponent->ForcedLOD, InComponent->LODBias)
	, MaxLOD(FMath::CeilLogTwo(InComponent->SubsectionSizeQuads + 1) - 1)
	, UseTessellationComponentScreenSizeFalloff(InComponent->GetCyLandProxy()->UseTessellationComponentScreenSizeFalloff)
	, NumWeightmapLayerAllocations(InComponent->WeightmapLayerAllocations.Num())
	, StaticLightingLOD(InComponent->GetCyLandProxy()->StaticLightingLOD)
	, WeightmapSubsectionOffset(InComponent->WeightmapSubsectionOffset)
	, FirstLOD(0)
	, LastLOD(MaxLOD)
	, ComponentMaxExtend(0.0f)
	, ComponentSquaredScreenSizeToUseSubSections(FMath::Square(InComponent->GetCyLandProxy()->ComponentScreenSizeToUseSubSections))
	, TessellationComponentSquaredScreenSize(FMath::Square(InComponent->GetCyLandProxy()->TessellationComponentScreenSize))
	, TessellationComponentScreenSizeFalloff(InComponent->GetCyLandProxy()->TessellationComponentScreenSizeFalloff)
	, NumSubsections(InComponent->NumSubsections)
	, SubsectionSizeQuads(InComponent->SubsectionSizeQuads)
	, SubsectionSizeVerts(InComponent->SubsectionSizeQuads + 1)
	, ComponentSizeQuads(InComponent->ComponentSizeQuads)
	, ComponentSizeVerts(InComponent->ComponentSizeQuads + 1)
	, SectionBase(InComponent->GetSectionBase())
	, CyLandComponent(InComponent)
	, WeightmapScaleBias(InComponent->WeightmapScaleBias)
	, WeightmapTextures(InComponent->WeightmapTextures)
	, NormalmapTexture(InComponent->GetHeightmap())
	, BaseColorForGITexture(InComponent->GIBakedBaseColorTexture)
	, HeightmapScaleBias(InComponent->HeightmapScaleBias)
	, XYOffsetmapTexture(InComponent->XYOffsetmapTexture)
	, SharedBuffersKey(0)
	, SharedBuffers(nullptr)
	, VertexFactory(nullptr)
	, ComponentLightInfo(nullptr)
#if WITH_EDITORONLY_DATA
	, EditToolRenderData(InComponent->EditToolRenderData)
	, LODFalloff_DEPRECATED(InComponent->GetCyLandProxy()->LODFalloff_DEPRECATED)
#endif
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	, CollisionMipLevel(InComponent->CollisionMipLevel)
	, SimpleCollisionMipLevel(InComponent->SimpleCollisionMipLevel)
	, CollisionResponse(InComponent->GetCyLandProxy()->BodyInstance.GetResponseToChannels())
#endif
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	, LightMapResolution(InComponent->GetStaticLightMapResolution())
#endif
{
#if !UE_BUILD_SHIPPING
	{
		static bool bStaticInit = false;
		if (!bStaticInit)
		{
			bStaticInit = true;
			CVarCyLandLODDistributionScale->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnLODDistributionScaleChanged));
			CVarCyLandLOD0DistributionScale->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnLODDistributionScaleChanged));
		}
	}
#endif

	const auto FeatureLevel = GetScene().GetFeatureLevel();

	if (FeatureLevel >= ERHIFeatureLevel::SM4)
	{
		if (InComponent->GetCyLandProxy()->bUseDynamicMaterialInstance)
		{
			AvailableMaterials.Append(InComponent->MaterialInstancesDynamic);
		}
		else
		{
			AvailableMaterials.Append(InComponent->MaterialInstances);
		}
	}
	else
	{
		AvailableMaterials.Append(InComponent->MobileMaterialInterfaces);
	}

	MaterialIndexToDisabledTessellationMaterial = InComponent->MaterialIndexToDisabledTessellationMaterial;
	LODIndexToMaterialIndex = InComponent->LODIndexToMaterialIndex;
	check(LODIndexToMaterialIndex.Num() == MaxLOD+1);

	if (!IsComponentLevelVisible())
	{
		bNeedsLevelAddedToWorldNotification = true;
	}

	SetLevelColor(FLinearColor(1.f, 1.f, 1.f));

	if (FeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		HeightmapTexture = nullptr;
		HeightmapSubsectionOffsetU = 0;
		HeightmapSubsectionOffsetV = 0;
	}
	else
	{
		HeightmapSubsectionOffsetU = ((float)(InComponent->SubsectionSizeQuads + 1) / (float)HeightmapTexture->GetSizeX());
		HeightmapSubsectionOffsetV = ((float)(InComponent->SubsectionSizeQuads + 1) / (float)HeightmapTexture->GetSizeY());
	}

	float ScreenSizeRatioDivider = FMath::Max(InComponent->GetCyLandProxy()->LOD0DistributionSetting * GCyLandLOD0DistributionScale, 1.01f);
	float CurrentScreenSizeRatio = 1.0f;

	LODScreenRatioSquared.AddUninitialized(MaxLOD + 1);

	// LOD 0 handling
	LODScreenRatioSquared[0] = FMath::Square(CurrentScreenSizeRatio);
	CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
	ScreenSizeRatioDivider = FMath::Max(InComponent->GetCyLandProxy()->LODDistributionSetting * GCyLandLODDistributionScale, 1.01f);

	// Other LODs
	for (int32 LODIndex = 1; LODIndex <= MaxLOD; ++LODIndex) // This should ALWAYS be calculated from the component size, not user MaxLOD override
	{
		LODScreenRatioSquared[LODIndex] = FMath::Square(CurrentScreenSizeRatio);
		CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
	}

	if (InComponent->GetCyLandProxy()->MaxLODLevel >= 0)
	{
		MaxLOD = FMath::Min<int8>(MaxLOD, InComponent->GetCyLandProxy()->MaxLODLevel);
	}

	FirstLOD = 0;
	LastLOD = MaxLOD;	// we always need to go to MaxLOD regardless of LODBias as we could need the lowest LODs due to streaming.

	// Make sure out LastLOD is > of MinStreamedLOD otherwise we would not be using the right LOD->MIP, the only drawback is a possible minor memory usage for overallocating static mesh element batch
	const int32 MinStreamedLOD = (HeightmapTexture != nullptr && HeightmapTexture->Resource != nullptr) ? FMath::Min<int32>(((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip(), FMath::CeilLogTwo(SubsectionSizeVerts) - 1) : 0;
	LastLOD = FMath::Max(MinStreamedLOD, LastLOD);

	ForcedLOD = ForcedLOD != INDEX_NONE ? FMath::Clamp<int32>(ForcedLOD, FirstLOD, LastLOD) : ForcedLOD;
	LODBias = FMath::Clamp<int8>(LODBias, -MaxLOD, MaxLOD);

	int8 LocalLODBias = LODBias + (int8)GCyLandMeshLODBias;
	MinValidLOD = FMath::Clamp<int8>(LocalLODBias, -MaxLOD, MaxLOD);
	MaxValidLOD = FMath::Min<int32>(MaxLOD, MaxLOD + LocalLODBias);

	ComponentMaxExtend = SubsectionSizeQuads * FMath::Max(InComponent->GetComponentTransform().GetScale3D().X, InComponent->GetComponentTransform().GetScale3D().Y);

	if (NumSubsections > 1)
	{
		FRotator ComponentRotator = CyLandComponent->GetComponentRotation();
		float SubSectionMaxExtend = ComponentMaxExtend / 2.0f;
		FVector ComponentTopLeftCorner = CyLandComponent->Bounds.Origin - ComponentRotator.RotateVector(FVector(SubSectionMaxExtend, SubSectionMaxExtend, 0.0f));

		SubSectionScreenSizeTestingPosition.AddUninitialized(MAX_SUBSECTION_COUNT);

		for (int32 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int32 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				int32 SubSectionIndex = SubX + SubY * NumSubsections;
				SubSectionScreenSizeTestingPosition[SubSectionIndex] = ComponentTopLeftCorner + ComponentRotator.RotateVector(FVector(ComponentMaxExtend * SubX, ComponentMaxExtend * SubY, 0.0f));
			}
		}
	}

	if (InComponent->StaticLightingResolution > 0.f)
	{
		StaticLightingResolution = InComponent->StaticLightingResolution;
	}
	else
	{
		StaticLightingResolution = InComponent->GetCyLandProxy()->StaticLightingResolution;
	}

	ComponentLightInfo = MakeUnique<FCyLandLCI>(InComponent);
	check(ComponentLightInfo);

	const bool bHasStaticLighting = ComponentLightInfo->GetLightMap() || ComponentLightInfo->GetShadowMap();

	// Check material usage
	if (ensure(AvailableMaterials.Num() > 0))
	{
		for (UMaterialInterface*& MaterialInterface : AvailableMaterials)
		{
			if (MaterialInterface == nullptr ||
				(bHasStaticLighting && !MaterialInterface->CheckMaterialUsage(MATUSAGE_StaticLighting)))
			{
				MaterialInterface = UMaterial::GetDefaultMaterial(MD_Surface);
			}
		}
	}
	else
	{
		AvailableMaterials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
	}

	MaterialRelevances.Reserve(AvailableMaterials.Num());

	for (UMaterialInterface*& MaterialInterface : AvailableMaterials)
	{
		UMaterial* CyLandMaterial = MaterialInterface != nullptr ? MaterialInterface->GetMaterial() : nullptr;

		if (CyLandMaterial != nullptr)
		{
			UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(MaterialInterface);

			// In some case it's possible that the Material Instance we have and the Material are not related, for example, in case where content was force deleted, we can have a MIC with no parent, so GetMaterial will fallback to the default material.
			// and since the MIC is not really valid, dont generate the relevance.
			if (MaterialInstance == nullptr || MaterialInstance->IsChildOf(CyLandMaterial))
			{
				MaterialRelevances.Add(MaterialInterface->GetRelevance(FeatureLevel));
			}

			bRequiresAdjacencyInformation |= MaterialSettingsRequireAdjacencyInformation_GameThread(MaterialInterface, XYOffsetmapTexture == nullptr ? &FCyLandVertexFactory::StaticType : &FCyLandXYOffsetVertexFactory::StaticType, InComponent->GetWorld()->FeatureLevel);

			bool HasTessellationEnabled = false;

			if (FeatureLevel >= ERHIFeatureLevel::SM4)
			{
				HasTessellationEnabled = CyLandMaterial->D3D11TessellationMode != EMaterialTessellationMode::MTM_NoTessellation;
			}

			MaterialHasTessellationEnabled.Add(HasTessellationEnabled);
		}
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST) || (UE_BUILD_SHIPPING && WITH_EDITOR)
	if (GIsEditor)
	{
		ACyLandProxy* Proxy = InComponent->GetCyLandProxy();
		// Try to find a color for level coloration.
		if (Proxy)
		{
			ULevel* Level = Proxy->GetLevel();
			ULevelStreaming* LevelStreaming = FLevelUtils::FindStreamingLevel(Level);
			if (LevelStreaming)
			{
				SetLevelColor(LevelStreaming->LevelColor);
			}
		}
	}
#endif

	const int8 SubsectionSizeLog2 = FMath::CeilLogTwo(InComponent->SubsectionSizeQuads + 1);
	SharedBuffersKey = (SubsectionSizeLog2 & 0xf) | ((NumSubsections & 0xf) << 4) |
		(FeatureLevel <= ERHIFeatureLevel::ES3_1 ? 0 : 1 << 30) | (XYOffsetmapTexture == nullptr ? 0 : 1 << 31);

	bSupportsHeightfieldRepresentation = true;

#if WITH_EDITOR
	for (auto& Allocation : InComponent->WeightmapLayerAllocations)
	{
		if (Allocation.LayerInfo != nullptr)
		{
			LayerColors.Add(Allocation.LayerInfo->LayerUsageDebugColor);
		}
	}
#endif
}

void FCyLandComponentSceneProxy::CreateRenderThreadResources()
{
	//UE_LOG(LogCyLand, Warning, TEXT("Creating RenderThreadResources...s"));
	check(HeightmapTexture != nullptr);

	if (IsComponentLevelVisible())
	{
		RegisterNeighbors();
	}

	auto FeatureLevel = GetScene().GetFeatureLevel();

	SharedBuffers = FCyLandComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey);
	if (SharedBuffers == nullptr)
	{
		SharedBuffers = new FCyLandSharedBuffers(
			SharedBuffersKey, SubsectionSizeQuads, NumSubsections,
			FeatureLevel, bRequiresAdjacencyInformation, /*NumOcclusionVertices*/ 0);

		FCyLandComponentSceneProxy::SharedBuffersMap.Add(SharedBuffersKey, SharedBuffers);

		if (!XYOffsetmapTexture)
		{
			FCyLandVertexFactory* CyLandVertexFactory = new FCyLandVertexFactory(FeatureLevel);
			CyLandVertexFactory->Data.PositionComponent = FVertexStreamComponent(SharedBuffers->VertexBuffer, 0, sizeof(FCyLandVertex), VET_Float4);
			CyLandVertexFactory->InitResource();
			SharedBuffers->VertexFactory = CyLandVertexFactory;
		}
		else
		{
			FCyLandXYOffsetVertexFactory* CyLandXYOffsetVertexFactory = new FCyLandXYOffsetVertexFactory(FeatureLevel);
			CyLandXYOffsetVertexFactory->Data.PositionComponent = FVertexStreamComponent(SharedBuffers->VertexBuffer, 0, sizeof(FCyLandVertex), VET_Float4);
			CyLandXYOffsetVertexFactory->InitResource();
			SharedBuffers->VertexFactory = CyLandXYOffsetVertexFactory;
		}
	}

	SharedBuffers->AddRef();

	if (bRequiresAdjacencyInformation)
	{
		if (SharedBuffers->AdjacencyIndexBuffers == nullptr)
		{
			ensure(SharedBuffers->NumIndexBuffers > 0);
			if (SharedBuffers->IndexBuffers[0])
			{
				// Recreate Index Buffers, this case happens only there are CyLand Components using different material (one uses tessellation, other don't use it) 
				if (SharedBuffers->bUse32BitIndices && !((FRawStaticIndexBuffer16or32<uint32>*)SharedBuffers->IndexBuffers[0])->Num())
				{
					SharedBuffers->CreateIndexBuffers<uint32>(FeatureLevel, bRequiresAdjacencyInformation);
				}
				else if (!((FRawStaticIndexBuffer16or32<uint16>*)SharedBuffers->IndexBuffers[0])->Num())
				{
					SharedBuffers->CreateIndexBuffers<uint16>(FeatureLevel, bRequiresAdjacencyInformation);
				}
			}

			SharedBuffers->AdjacencyIndexBuffers = new FCyLandSharedAdjacencyIndexBuffer(SharedBuffers);
		}

		// Delayed Initialize for IndexBuffers
		for (int32 i = 0; i < SharedBuffers->NumIndexBuffers; i++)
		{
			SharedBuffers->IndexBuffers[i]->InitResource();
		}
	}

	// Assign vertex factory
	VertexFactory = SharedBuffers->VertexFactory;

	//UE_LOG(LogCyLand, Warning, TEXT("Creating RenderThreadResources over... %d"), VertexFactory!=NULL);
	// Assign CyLandCyUniformShaderParameters
	CyLandCyUniformShaderParameters.InitResource();

#if WITH_EDITOR
	// Create MeshBatch for grass rendering
	if (SharedBuffers->GrassIndexBuffer)
	{
		const int32 NumMips = FMath::CeilLogTwo(SubsectionSizeVerts);
		GrassMeshBatch.Elements.Empty(NumMips);
		GrassMeshBatch.Elements.AddDefaulted(NumMips);
		GrassBatchParams.Empty(NumMips);
		GrassBatchParams.AddDefaulted(NumMips);
		
		// Grass is being generated using LOD0 material only
		FMaterialRenderProxy* RenderProxy = AvailableMaterials[LODIndexToMaterialIndex[0]]->GetRenderProxy();
		GrassMeshBatch.VertexFactory = VertexFactory;
		GrassMeshBatch.MaterialRenderProxy = RenderProxy;
		GrassMeshBatch.LCI = nullptr;
		GrassMeshBatch.ReverseCulling = false;
		GrassMeshBatch.CastShadow = false;
		GrassMeshBatch.Type = PT_PointList;
		GrassMeshBatch.DepthPriorityGroup = SDPG_World;

		// Combined grass rendering batch element
		FMeshBatchElement* GrassBatchElement = &GrassMeshBatch.Elements[0];
		FCyLandBatchElementParams* BatchElementParams = &GrassBatchParams[0];
		BatchElementParams->LocalToWorldNoScalingPtr = &LocalToWorldNoScaling;
		BatchElementParams->CyLandCyUniformShaderParametersResource = &CyLandCyUniformShaderParameters;
		BatchElementParams->SceneProxy = this;
		BatchElementParams->SubX = -1;
		BatchElementParams->SubY = -1;
		BatchElementParams->CurrentLOD = 0;
		GrassBatchElement->UserData = BatchElementParams;
		GrassBatchElement->PrimitiveUniformBuffer = GetUniformBuffer();
		GrassBatchElement->IndexBuffer = SharedBuffers->GrassIndexBuffer;
		GrassBatchElement->NumPrimitives = FMath::Square(NumSubsections) * FMath::Square(SubsectionSizeVerts);
		GrassBatchElement->FirstIndex = 0;
		GrassBatchElement->MinVertexIndex = 0;
		GrassBatchElement->MaxVertexIndex = SharedBuffers->NumVertices - 1;

		for (int32 Mip = 1; Mip < NumMips; ++Mip)
		{
			const int32 MipSubsectionSizeVerts = SubsectionSizeVerts >> Mip;

			FMeshBatchElement* CollisionBatchElement = &GrassMeshBatch.Elements[Mip];
			*CollisionBatchElement = *GrassBatchElement;
			FCyLandBatchElementParams* CollisionBatchElementParams = &GrassBatchParams[Mip];
			*CollisionBatchElementParams = *BatchElementParams;
			CollisionBatchElementParams->CurrentLOD = Mip;
			CollisionBatchElement->UserData = CollisionBatchElementParams;
			CollisionBatchElement->NumPrimitives = FMath::Square(NumSubsections) * FMath::Square(MipSubsectionSizeVerts);
			CollisionBatchElement->FirstIndex = SharedBuffers->GrassIndexMipOffsets[Mip];
		}
	}
#endif
}

void FCyLandComponentSceneProxy::OnLevelAddedToWorld()
{
	RegisterNeighbors();
}

FCyLandComponentSceneProxy::~FCyLandComponentSceneProxy()
{
	UnregisterNeighbors();

	// CyFree the subsection uniform buffer
	CyLandCyUniformShaderParameters.ReleaseResource();

	if (SharedBuffers)
	{
		check(SharedBuffers == FCyLandComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey));
		if (SharedBuffers->Release() == 0)
		{
			FCyLandComponentSceneProxy::SharedBuffersMap.Remove(SharedBuffersKey);
		}
		SharedBuffers = nullptr;
	}
}

bool FCyLandComponentSceneProxy::CanBeOccluded() const
{
	for (const FMaterialRelevance& Relevance : MaterialRelevances)
	{
		if (!Relevance.bDisableDepthTest)
		{
			return true;
		}
	}

	return false;
}

FPrimitiveViewRelevance FCyLandComponentSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	const bool bCollisionView = (View->Family->EngineShowFlags.CollisionVisibility || View->Family->EngineShowFlags.CollisionPawn);
	Result.bDrawRelevance = (IsShown(View) || bCollisionView) && View->Family->EngineShowFlags.Landscape;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	Result.bUseCustomViewData = true;

	auto FeatureLevel = View->GetFeatureLevel();

#if WITH_EDITOR
	if (!GCyLandEditModeActive)
	{
		// No tools to render, just use the cached material relevance.
#endif
		for (const FMaterialRelevance& MaterialRelevance : MaterialRelevances)
		{
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
		}

#if WITH_EDITOR
	}
	else
	{
		for (const FMaterialRelevance& MaterialRelevance : MaterialRelevances)
		{
			// Also add the tool material(s)'s relevance to the MaterialRelevance
			FMaterialRelevance ToolRelevance = MaterialRelevance;

			// Tool brushes and Gizmo
			if (EditToolRenderData.ToolMaterial)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= EditToolRenderData.ToolMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			if (EditToolRenderData.GizmoMaterial)
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= EditToolRenderData.GizmoMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			// Region selection
			if (EditToolRenderData.SelectedType)
			{
				if ((GCyLandEditRenderMode & ECyLandEditRenderMode::SelectRegion) && (EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_REGION)
					&& !(GCyLandEditRenderMode & ECyLandEditRenderMode::Mask) && GSelectionRegionMaterial)
				{
					Result.bDynamicRelevance = true;
					ToolRelevance |= GSelectionRegionMaterial->GetRelevance_Concurrent(FeatureLevel);
				}
				if ((GCyLandEditRenderMode & ECyLandEditRenderMode::SelectComponent) && (EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_COMPONENT) && GSelectionColorMaterial)
				{
					Result.bDynamicRelevance = true;
					ToolRelevance |= GSelectionColorMaterial->GetRelevance_Concurrent(FeatureLevel);
				}
			}

			// Mask
			if ((GCyLandEditRenderMode & ECyLandEditRenderMode::Mask) && GMaskRegionMaterial != nullptr &&
				(((EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_REGION)) || (!(GCyLandEditRenderMode & ECyLandEditRenderMode::InvertedMask))))
			{
				Result.bDynamicRelevance = true;
				ToolRelevance |= GMaskRegionMaterial->GetRelevance_Concurrent(FeatureLevel);
			}

			ToolRelevance.SetPrimitiveViewRelevance(Result);
		}
	}

	// Various visualizations need to render using dynamic relevance
	if ((View->Family->EngineShowFlags.Bounds && IsSelected()) ||
		GCyLandDebugOptions.bShowPatches)
	{
		Result.bDynamicRelevance = true;
	}
#endif

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const bool bInCollisionView = View->Family->EngineShowFlags.CollisionVisibility || View->Family->EngineShowFlags.CollisionPawn;
#endif

	// Use the dynamic path for rendering landscape components pass only for Rich Views or if the static path is disabled for debug.
	if (IsRichView(*View->Family) ||
#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		bInCollisionView ||
#endif
		GCyLandDebugOptions.bDisableStatic ||
		View->Family->EngineShowFlags.Wireframe ||
#if WITH_EDITOR
		(IsSelected() && !GCyLandEditModeActive) ||
		GCyLandViewMode != ECyLandViewMode::Normal ||
#else
		IsSelected() ||
#endif
		!IsStaticPathAvailable())
	{
		Result.bDynamicRelevance = true;
	}
	else
	{
		Result.bStaticRelevance = true;
	}

	Result.bShadowRelevance = (GAllowCyLandShadows > 0) && IsShadowCast(View);
	return Result;
}

/**
*	Determines the relevance of this primitive's elements to the given light.
*	@param	LightSceneProxy			The light to determine relevance for
*	@param	bDynamic (output)		The light is dynamic for this primitive
*	@param	bRelevant (output)		The light is relevant for this primitive
*	@param	bLightMapped (output)	The light is light mapped for this primitive
*/
void FCyLandComponentSceneProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	// Attach the light to the primitive's static meshes.
	bDynamic = true;
	bRelevant = false;
	bLightMapped = true;
	bShadowMapped = true;

	if (ComponentLightInfo)
	{
		ELightInteractionType InteractionType = ComponentLightInfo->GetInteraction(LightSceneProxy).GetType();

		if (InteractionType != LIT_CachedIrrelevant)
		{
			bRelevant = true;
		}

		if (InteractionType != LIT_CachedLightMap && InteractionType != LIT_CachedIrrelevant)
		{
			bLightMapped = false;
		}

		if (InteractionType != LIT_Dynamic)
		{
			bDynamic = false;
		}

		if (InteractionType != LIT_CachedSignedDistanceFieldShadowMap2D)
		{
			bShadowMapped = false;
		}
	}
	else
	{
		bRelevant = true;
		bLightMapped = false;
	}
}

SIZE_T FCyLandComponentSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

FLightInteraction FCyLandComponentSceneProxy::FCyLandLCI::GetInteraction(const class FLightSceneProxy* LightSceneProxy) const
{
	// ask base class
	ELightInteractionType LightInteraction = GetStaticInteraction(LightSceneProxy, IrrelevantLights);

	if (LightInteraction != LIT_MAX)
	{
		return FLightInteraction(LightInteraction);
	}

	// Use dynamic lighting if the light doesn't have static lighting.
	return FLightInteraction::Dynamic();
}

#if WITH_EDITOR
namespace DebugColorMask
{
	const FLinearColor Masks[5] =
	{
		FLinearColor(1.f, 0.f, 0.f, 0.f),
		FLinearColor(0.f, 1.f, 0.f, 0.f),
		FLinearColor(0.f, 0.f, 1.f, 0.f),
		FLinearColor(0.f, 0.f, 0.f, 1.f),
		FLinearColor(0.f, 0.f, 0.f, 0.f)
	};
};
#endif

void FCyLandComponentSceneProxy::OnTransformChanged()
{
	// Set Lightmap ScaleBias
	int32 PatchExpandCountX = 0;
	int32 PatchExpandCountY = 0;
	int32 DesiredSize = 1; // output by GetTerrainExpandPatchCount but not used below
	const float LightMapRatio = ::GetTerrainExpandPatchCount(StaticLightingResolution, PatchExpandCountX, PatchExpandCountY, ComponentSizeQuads, (NumSubsections * (SubsectionSizeQuads + 1)), DesiredSize, StaticLightingLOD);
	const float LightmapLODScaleX = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountX);
	const float LightmapLODScaleY = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountY);
	const float LightmapBiasX = PatchExpandCountX * LightmapLODScaleX;
	const float LightmapBiasY = PatchExpandCountY * LightmapLODScaleY;
	const float LightmapScaleX = LightmapLODScaleX * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapScaleY = LightmapLODScaleY * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapExtendFactorX = (float)SubsectionSizeQuads * LightmapScaleX;
	const float LightmapExtendFactorY = (float)SubsectionSizeQuads * LightmapScaleY;

	// cache component's WorldToLocal
	FMatrix LtoW = GetLocalToWorld();
	WorldToLocal = LtoW.InverseFast();

	// cache component's LocalToWorldNoScaling
	LocalToWorldNoScaling = LtoW;
	LocalToWorldNoScaling.RemoveScaling();

	// Set FCyLandCyUniformVSParameters for this subsection
	FCyLandCyUniformShaderParameters CyLandParams;
	CyLandParams.HeightmapUVScaleBias = HeightmapScaleBias;
	CyLandParams.WeightmapUVScaleBias = WeightmapScaleBias;
	CyLandParams.LocalToWorldNoScaling = LocalToWorldNoScaling;

	CyLandParams.LandscapeLightmapScaleBias = FVector4(
		LightmapScaleX,
		LightmapScaleY,
		LightmapBiasY,
		LightmapBiasX);
	CyLandParams.SubsectionSizeVertsLayerUVPan = FVector4(
		SubsectionSizeVerts,
		1.f / (float)SubsectionSizeQuads,
		SectionBase.X,
		SectionBase.Y
	);
	CyLandParams.SubsectionOffsetParams = FVector4(
		HeightmapSubsectionOffsetU,
		HeightmapSubsectionOffsetV,
		WeightmapSubsectionOffset,
		SubsectionSizeQuads
	);
	CyLandParams.LightmapSubsectionOffsetParams = FVector4(
		LightmapExtendFactorX,
		LightmapExtendFactorY,
		0,
		0
	);

	CyLandCyUniformShaderParameters.SetContents(CyLandParams);
}

float FCyLandComponentSceneProxy::GetComponentScreenSize(const FSceneView* View, const FVector& Origin, float MaxExtend, float ElementRadius) const
{
	FVector CameraOrigin = View->ViewMatrices.GetViewOrigin();
	FMatrix ProjMatrix = View->ViewMatrices.GetProjectionMatrix();

	const FVector OriginToCamera = (CameraOrigin - Origin).GetAbs();
	const FVector ClosestPoint = OriginToCamera.ComponentMin(FVector(MaxExtend));
	const float DistSquared = (OriginToCamera - ClosestPoint).SizeSquared();

	// Get projection multiple accounting for view scaling.
	const float ScreenMultiple = FMath::Max(0.5f * ProjMatrix.M[0][0], 0.5f * ProjMatrix.M[1][1]);

	// Calculate screen-space projected radius
	float SquaredScreenRadius = FMath::Square(ScreenMultiple * ElementRadius) / FMath::Max(1.0f, DistSquared);

	return FMath::Min(SquaredScreenRadius * 2.0f, 1.0f);
}

void FCyLandComponentSceneProxy::BuildDynamicMeshElement(const FViewCustomDataLOD* InPrimitiveCustomData, bool InToolMesh, bool InHasTessellation, bool InDisableTessellation, FMeshBatch& OutMeshBatch, TArray<FCyLandBatchElementParams, SceneRenderingAllocator>& OutStaticBatchParamArray) const
{
	//UE_LOG(LogCyLand, Warning, TEXT("ACyLand BuildDynamicMeshElement  BuildDynamicMeshElement"));
	if (AvailableMaterials.Num() == 0)
	{
		return;
	}

	const int8 CurrentLODIndex = InPrimitiveCustomData->SubSections[0].BatchElementCurrentLOD;
	int8 MaterialIndex = LODIndexToMaterialIndex.IsValidIndex(CurrentLODIndex) ? LODIndexToMaterialIndex[CurrentLODIndex] : INDEX_NONE;
	UMaterialInterface* SelectedMaterial = MaterialIndex != INDEX_NONE ? AvailableMaterials[MaterialIndex] : nullptr;

	if (InHasTessellation && MaterialIndex != INDEX_NONE)
	{		
		if (InDisableTessellation && MaterialIndexToDisabledTessellationMaterial.IsValidIndex(MaterialIndex))
		{
			SelectedMaterial = AvailableMaterials[MaterialIndexToDisabledTessellationMaterial[MaterialIndex]];
		}
	}

	// this is really not normal that we have no material at this point, so do not continue
	if (SelectedMaterial == nullptr)
	{
		return;
	}

	// Could be different from bRequiresAdjacencyInformation during shader compilation
	bool bCurrentRequiresAdjacencyInformation = !InToolMesh && MaterialRenderingRequiresAdjacencyInformation_RenderingThread(SelectedMaterial, VertexFactory->GetType(), GetScene().GetFeatureLevel());

	if (bCurrentRequiresAdjacencyInformation)
	{
		check(SharedBuffers->AdjacencyIndexBuffers);
	}

	OutMeshBatch.VertexFactory = VertexFactory;
	OutMeshBatch.MaterialRenderProxy = SelectedMaterial->GetRenderProxy();
	OutMeshBatch.LCI = ComponentLightInfo.Get();
	OutMeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
	OutMeshBatch.CastShadow = InToolMesh ? false : true;
	OutMeshBatch.bUseAsOccluder = ShouldUseAsOccluder() && GetScene().GetShadingPath() == EShadingPath::Deferred && !IsMovable();
	OutMeshBatch.bUseForMaterial = true;
	OutMeshBatch.Type = bCurrentRequiresAdjacencyInformation ? PT_12_ControlPointPatchList : PT_TriangleList;
	OutMeshBatch.LODIndex = 0;

	OutMeshBatch.Elements.Empty();
	//UE_LOG(LogCyLand, Warning, TEXT("ACyLand BuildDynamicMeshElement  OutMeshBatch prepared !"));
	if (NumSubsections > 1 && !InPrimitiveCustomData->UseCombinedMeshBatch)
	{
		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				const int8 SubSectionIdx = SubX + SubY * NumSubsections;
				const int8 CurrentLOD = InPrimitiveCustomData->SubSections[SubSectionIdx].BatchElementCurrentLOD;

				FMeshBatchElement BatchElement;
				FCyLandBatchElementParams& BatchElementParams = OutStaticBatchParamArray[SubSectionIdx];

				if (!InToolMesh)
				{
					BatchElementParams.LocalToWorldNoScalingPtr = &LocalToWorldNoScaling;
					BatchElementParams.CyLandCyUniformShaderParametersResource = &CyLandCyUniformShaderParameters;
					BatchElementParams.SceneProxy = this;
					BatchElementParams.SubX = SubX;
					BatchElementParams.SubY = SubY;
					BatchElementParams.CurrentLOD = CurrentLOD;
				}

				BatchElement.UserData = &BatchElementParams;
				BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

				int32 LodSubsectionSizeVerts = (SubsectionSizeVerts >> CurrentLOD);
				uint32 NumPrimitives = FMath::Square((LodSubsectionSizeVerts - 1)) * 2;

				if (bCurrentRequiresAdjacencyInformation)
				{
					check(SharedBuffers->AdjacencyIndexBuffers);
					BatchElement.IndexBuffer = SharedBuffers->AdjacencyIndexBuffers->IndexBuffers[CurrentLOD];
					BatchElement.FirstIndex = (SubX + SubY * NumSubsections) * NumPrimitives * 12;
				}
				else
				{
					BatchElement.IndexBuffer = SharedBuffers->IndexBuffers[CurrentLOD];
					BatchElement.FirstIndex = (SubX + SubY * NumSubsections) * NumPrimitives * 3;
				}
				BatchElement.NumPrimitives = NumPrimitives;
				BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[CurrentLOD].MinIndex[SubX][SubY];
				BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[CurrentLOD].MaxIndex[SubX][SubY];


#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				// We simplify this by considering only the biggest LOD index for this mesh element.
				OutMeshBatch.VisualizeLODIndex = FMath::Max(OutMeshBatch.VisualizeLODIndex, CurrentLOD);
#endif

				OutMeshBatch.Elements.Add(BatchElement);
			}
		}
	}
	else
	{
		FMeshBatchElement BatchElement;

		if (InToolMesh)
		{
			// Reuse the params for the tool mesh
			BatchElement.UserData = &OutStaticBatchParamArray[0];
		}
		else
		{
			FCyLandBatchElementParams& BatchElementParams = OutStaticBatchParamArray[0];
			BatchElementParams.CyLandCyUniformShaderParametersResource = &CyLandCyUniformShaderParameters;
			BatchElementParams.LocalToWorldNoScalingPtr = &LocalToWorldNoScaling;
			BatchElementParams.SceneProxy = this;
			BatchElementParams.SubX = -1;
			BatchElementParams.SubY = -1;
			BatchElementParams.CurrentLOD = CurrentLODIndex;

			BatchElement.UserData = &BatchElementParams;
		}

		// Combined batch element
		int32 LodSubsectionSizeVerts = SubsectionSizeVerts >> CurrentLODIndex;

		BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
		BatchElement.IndexBuffer = bCurrentRequiresAdjacencyInformation ? SharedBuffers->AdjacencyIndexBuffers->IndexBuffers[CurrentLODIndex] : SharedBuffers->IndexBuffers[CurrentLODIndex];
		BatchElement.NumPrimitives = FMath::Square((LodSubsectionSizeVerts - 1)) * FMath::Square(NumSubsections) * 2;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[CurrentLODIndex].MinIndexFull;
		BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[CurrentLODIndex].MaxIndexFull;

		OutMeshBatch.Elements.Add(BatchElement);
	}
}

bool FCyLandComponentSceneProxy::GetMeshElement(bool UseSeperateBatchForShadow, bool ShadowOnly, bool HasTessellation, int8 InLODIndex, UMaterialInterface* InMaterialInterface, FMeshBatch& OutMeshBatch, TArray<FCyLandBatchElementParams>& OutStaticBatchParamArray) const
{
	if (InMaterialInterface == nullptr)
	{
		return false;
	}

	// Could be different from bRequiresAdjacencyInformation during shader compilation
	bool bCurrentRequiresAdjacencyInformation = MaterialRenderingRequiresAdjacencyInformation_RenderingThread(InMaterialInterface, VertexFactory->GetType(), GetScene().GetFeatureLevel());

	if (bCurrentRequiresAdjacencyInformation)
	{
		check(SharedBuffers->AdjacencyIndexBuffers);
	}

	OutMeshBatch.VertexFactory = VertexFactory;
	OutMeshBatch.MaterialRenderProxy = InMaterialInterface->GetRenderProxy();

	OutMeshBatch.LCI = ComponentLightInfo.Get();
	OutMeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
	OutMeshBatch.CastShadow = UseSeperateBatchForShadow ? ShadowOnly : true;
	OutMeshBatch.bUseForDepthPass = (UseSeperateBatchForShadow ? !ShadowOnly : true);
	OutMeshBatch.bUseAsOccluder = (UseSeperateBatchForShadow ? !ShadowOnly : true) && ShouldUseAsOccluder() && GetScene().GetShadingPath() == EShadingPath::Deferred && !IsMovable();
	OutMeshBatch.bUseForMaterial = (UseSeperateBatchForShadow ? !ShadowOnly : true);
	OutMeshBatch.Type = bCurrentRequiresAdjacencyInformation ? PT_12_ControlPointPatchList : PT_TriangleList;
	OutMeshBatch.DepthPriorityGroup = SDPG_World;
	OutMeshBatch.LODIndex = InLODIndex;
	OutMeshBatch.bRequiresPerElementVisibility = true;
	OutMeshBatch.bDitheredLODTransition = false;

	if (OutMeshBatch.CastShadow)
	{
		float QuadWorldSize = (ComponentMaxExtend * 2.0f) / (SubsectionSizeQuads * NumSubsections);
		FVector QuadSize = FVector(QuadWorldSize, QuadWorldSize, HasTessellation ? OutMeshBatch.MaterialRenderProxy->GetMaterial(GetScene().GetFeatureLevel())->GetMaxDisplacement() : 0.0f);

		OutMeshBatch.TessellationDisablingShadowMapMeshSize = QuadSize.Size();
	}

	int32 BatchElementSize = NumSubsections == 1 ? 1 : MAX_SUBSECTION_COUNT + 1;
	OutMeshBatch.Elements.Empty(FMath::Max(LastLOD - FirstLOD, 1) * BatchElementSize);

	for (int32 i = FirstLOD; i <= LastLOD; ++i)
	{
		int32 LodSubsectionSizeVerts = SubsectionSizeVerts >> i;

		if (NumSubsections > 1 && ForcedLOD < 0)
		{
			uint32 NumPrimitivesPerSection = FMath::Square((LodSubsectionSizeVerts - 1)) * 2;

			// Per-subsection batch elements
			for (int32 SubY = 0; SubY < NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < NumSubsections; SubX++)
				{
					FCyLandBatchElementParams* BatchElementParams = new(OutStaticBatchParamArray) FCyLandBatchElementParams;
					BatchElementParams->CyLandCyUniformShaderParametersResource = &CyLandCyUniformShaderParameters;
					BatchElementParams->LocalToWorldNoScalingPtr = &LocalToWorldNoScaling;
					BatchElementParams->SceneProxy = this;
					BatchElementParams->SubX = SubX;
					BatchElementParams->SubY = SubY;
					BatchElementParams->CurrentLOD = i;

					FMeshBatchElement BatchElement;
					BatchElement.UserData = BatchElementParams;
					BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();

					if (bCurrentRequiresAdjacencyInformation)
					{
						BatchElement.IndexBuffer = SharedBuffers->AdjacencyIndexBuffers->IndexBuffers[i];
						BatchElement.FirstIndex = (SubX + SubY * NumSubsections) * NumPrimitivesPerSection * 12;
					}
					else
					{
						BatchElement.IndexBuffer = SharedBuffers->IndexBuffers[i];
						BatchElement.FirstIndex = (SubX + SubY * NumSubsections) * NumPrimitivesPerSection * 3;
					}
					BatchElement.NumPrimitives = NumPrimitivesPerSection;
					BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[i].MinIndex[SubX][SubY];
					BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[i].MaxIndex[SubX][SubY];

					OutMeshBatch.Elements.Add(BatchElement);
				}
			}
		}

		// Combined batch element
		FCyLandBatchElementParams* BatchElementParams = new(OutStaticBatchParamArray) FCyLandBatchElementParams;
		BatchElementParams->CyLandCyUniformShaderParametersResource = &CyLandCyUniformShaderParameters;
		BatchElementParams->LocalToWorldNoScalingPtr = &LocalToWorldNoScaling;
		BatchElementParams->SceneProxy = this;
		BatchElementParams->SubX = -1;
		BatchElementParams->SubY = -1;
		BatchElementParams->CurrentLOD = i;

		FMeshBatchElement BatchElement;

		BatchElement.UserData = BatchElementParams;
		BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
		BatchElement.IndexBuffer = bCurrentRequiresAdjacencyInformation ? SharedBuffers->AdjacencyIndexBuffers->IndexBuffers[i] : SharedBuffers->IndexBuffers[i];
		BatchElement.NumPrimitives = FMath::Square((LodSubsectionSizeVerts - 1)) * FMath::Square(NumSubsections) * 2;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = SharedBuffers->IndexRanges[i].MinIndexFull;
		BatchElement.MaxVertexIndex = SharedBuffers->IndexRanges[i].MaxIndexFull;

		OutMeshBatch.Elements.Add(BatchElement);
	}

	OutMeshBatch.Elements.Shrink();

	return true;
}

void FCyLandComponentSceneProxy::ApplyWorldOffset(FVector InOffset)
{
	FPrimitiveSceneProxy::ApplyWorldOffset(InOffset);

	if (NumSubsections > 1)
	{
		for (int32 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int32 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				int32 SubSectionIndex = SubX + SubY * NumSubsections;
				SubSectionScreenSizeTestingPosition[SubSectionIndex] += InOffset;
			}
		}
	}
}

void FCyLandComponentSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	if (AvailableMaterials.Num() == 0)
	{
		return;
	}

	const int32 NumBatchesPerLOD = NumSubsections == 1 ? 1 : MAX_SUBSECTION_COUNT + 1;
	const int32 TessellatedRequiredBatchCount = 4;
	const int32 NoTessellatedRequiredBatchCount = 1;
	int32 MaterialCount = AvailableMaterials.Num();
	int32 BatchCount = 0;
	const auto FeatureLevel = GetScene().GetFeatureLevel();

	for (int32 i = 0; i < AvailableMaterials.Num(); ++i)
	{
		UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[i]);

		if (CyLandMIC == nullptr)
		{
			UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[i]);

			if (CyLandMID != nullptr)
			{
				CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
			}
		}

		bool HasTessellationEnabled = MaterialHasTessellationEnabled[i];
		check(HasTessellationEnabled && CyLandMIC != nullptr || !HasTessellationEnabled);

		BatchCount += HasTessellationEnabled && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation) ? TessellatedRequiredBatchCount : NoTessellatedRequiredBatchCount;

		// Remove material with disabled tessellation as they were generated
		if (HasTessellationEnabled && (CyLandMIC != nullptr && CyLandMIC->bDisableTessellation))
		{
			--MaterialCount;
		}
	}		

	StaticBatchParamArray.Empty((1 + LastLOD - FirstLOD) * NumBatchesPerLOD * BatchCount);
	MaterialIndexToStaticMeshBatchLOD.SetNumUninitialized(MaterialCount);

	int32 CurrentLODIndex = 0;

	PDI->ReserveMemoryForMeshes(MaterialCount);

	for (int32 i = 0; i < MaterialCount; ++i)
	{
		if (AvailableMaterials[i] == nullptr)
		{
			continue;
		}

		UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(AvailableMaterials[i]);

		bool HasTessellationEnabled = (FeatureLevel >= ERHIFeatureLevel::SM4) ? MaterialInstance != nullptr && MaterialInstance->GetMaterial()->D3D11TessellationMode != EMaterialTessellationMode::MTM_NoTessellation && MaterialIndexToDisabledTessellationMaterial[i] != INDEX_NONE : false;

		// Only add normal batch index (for each material)
		MaterialIndexToStaticMeshBatchLOD[i] = CurrentLODIndex;

		// Default Batch, tessellated if enabled
		FMeshBatch NormalMeshBatch;

		if (GetMeshElement(false, false, HasTessellationEnabled, CurrentLODIndex++, AvailableMaterials[i], NormalMeshBatch, StaticBatchParamArray))
		{
			PDI->DrawMesh(NormalMeshBatch, FLT_MAX);
		}

		if (HasTessellationEnabled)
		{
			UMaterialInstance* NonTessellatedCyLandMI = Cast<UMaterialInstance>(AvailableMaterials[MaterialIndexToDisabledTessellationMaterial[i]]);

			// Make sure that the Material instance we are going to use has the tessellation disabled
			UMaterialInstanceDynamic* NonTessellatedCyLandMID = Cast<UMaterialInstanceDynamic>(NonTessellatedCyLandMI);
			UCyLandMaterialInstanceConstant* NonTessellatedCyLandMIC = Cast<UCyLandMaterialInstanceConstant>(NonTessellatedCyLandMI);

			if (NonTessellatedCyLandMID != nullptr)
			{
				NonTessellatedCyLandMIC = Cast<UCyLandMaterialInstanceConstant>(NonTessellatedCyLandMID->Parent);
			}

			check(NonTessellatedCyLandMIC != nullptr && NonTessellatedCyLandMIC->bDisableTessellation);

			// Default Batch, no Tessellation enabled
			FMeshBatch NonTesselatedNormalMeshBatch;

			if (GetMeshElement(true, false, false, CurrentLODIndex++, NonTessellatedCyLandMI, NonTesselatedNormalMeshBatch, StaticBatchParamArray))
			{
				PDI->DrawMesh(NonTesselatedNormalMeshBatch, FLT_MAX);
			}

			// Shadow Batch, tessellated if enabled
			FMeshBatch ShadowMeshBatch;

			if (GetMeshElement(true, true, HasTessellationEnabled, CurrentLODIndex++, MaterialInstance, ShadowMeshBatch, StaticBatchParamArray))
			{
				PDI->DrawMesh(ShadowMeshBatch, FLT_MAX);
			}

			// Shadow Batch, no tessellation enabled
			FMeshBatch NonTesselatedShadowMeshBatch;

			if (GetMeshElement(true, true, false, CurrentLODIndex++, NonTessellatedCyLandMI, NonTesselatedShadowMeshBatch, StaticBatchParamArray))
			{
				PDI->DrawMesh(NonTesselatedShadowMeshBatch, FLT_MAX);
			}
		}
	}
}

void FCyLandComponentSceneProxy::CalculateLODFromScreenSize(const FSceneView& InView, float InMeshScreenSizeSquared, float InViewLODScale, int32 InSubSectionIndex, FViewCustomDataLOD& InOutLODData) const
{
	// Handle general LOD override
	float PreferedLOD = (float)GetCVarForceLOD();

#if WITH_EDITOR
	if (InView.Family->LandscapeLODOverride >= 0)
	{
		PreferedLOD = (float)InView.Family->LandscapeLODOverride;
	}
#endif

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (InView.Family->EngineShowFlags.CollisionVisibility || InView.Family->EngineShowFlags.CollisionPawn)
	{
		const bool bDrawSimpleCollision = InView.Family->EngineShowFlags.CollisionPawn        && CollisionResponse.GetResponse(ECC_Pawn) != ECR_Ignore;
		const bool bDrawComplexCollision = InView.Family->EngineShowFlags.CollisionVisibility && CollisionResponse.GetResponse(ECC_Visibility) != ECR_Ignore;

		if (bDrawSimpleCollision)
		{
			PreferedLOD = FMath::Max(CollisionMipLevel, SimpleCollisionMipLevel);
		}
		else if (bDrawComplexCollision)
		{
			PreferedLOD = CollisionMipLevel;
		}
	}
#endif

	if (ForcedLOD >= 0)
	{
		PreferedLOD = (float)ForcedLOD;
	}

	int8 MinStreamedLOD = HeightmapTexture ? FMath::Min<int8>(((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip(), FMath::CeilLogTwo(SubsectionSizeVerts) - 1) : 0;
	MinStreamedLOD = FMath::Min(MinStreamedLOD, MaxLOD); // We can't go above MaxLOD even for texture streaming

	int8 LocalLODBias = LODBias + (int8)GCyLandMeshLODBias;
	FViewCustomDataSubSectionLOD& SubSectionLODData = InOutLODData.SubSections[InSubSectionIndex];

	if (PreferedLOD >= 0.0f)
	{
		PreferedLOD = FMath::Clamp<float>(PreferedLOD + LocalLODBias, FMath::Max((float)MinStreamedLOD, MinValidLOD), FMath::Min((float)LastLOD, MaxValidLOD));
	}
	else
	{
		PreferedLOD = FMath::Clamp<float>(ComputeBatchElementCurrentLOD(GetLODFromScreenSize(InMeshScreenSizeSquared, InViewLODScale), InMeshScreenSizeSquared) + LocalLODBias, FMath::Max((float)MinStreamedLOD, MinValidLOD), FMath::Min((float)LastLOD, MaxValidLOD));
	}

	check(PreferedLOD != -1.0f && PreferedLOD <= MaxLOD);
	SubSectionLODData.fBatchElementCurrentLOD = PreferedLOD;
	SubSectionLODData.BatchElementCurrentLOD = FMath::FloorToInt(PreferedLOD);
}

uint64 FCyLandVertexFactory::GetStaticBatchElementVisibility(const FSceneView& InView, const FMeshBatch* InBatch, const void* InViewCustomData) const
{
	const FCyLandComponentSceneProxy* SceneProxy = ((FCyLandBatchElementParams*)InBatch->Elements[0].UserData)->SceneProxy;
	return SceneProxy->GetStaticBatchElementVisibility(InView, InBatch, InViewCustomData);
}

uint64 FCyLandComponentSceneProxy::GetStaticBatchElementVisibility(const FSceneView& InView, const FMeshBatch* InBatch, const void* InViewCustomData) const
{
	uint64 BatchesToRenderMask = 0;

	SCOPE_CYCLE_COUNTER(STAT_CyLandStaticDrawLODTime);

	const void* ViewCustomData = InViewCustomData != nullptr ? InViewCustomData : InView.GetCustomData(GetPrimitiveSceneInfo()->GetIndex());

	if (ViewCustomData != nullptr)
	{
		const FViewCustomDataLOD* CurrentLODData = (const FViewCustomDataLOD*)ViewCustomData;
		const auto FeatureLevel = InView.GetFeatureLevel();

		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			const int8 CurrentLODIndex = CurrentLODData->SubSections[0].BatchElementCurrentLOD;
			UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

			if (CyLandMIC == nullptr)
			{
				UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

				if (CyLandMID != nullptr)
				{
					CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
				}
			}

			bool HasTessellationEnabled = MaterialHasTessellationEnabled[LODIndexToMaterialIndex[CurrentLODIndex]] && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation);

			if (HasTessellationEnabled)
			{
				INC_DWORD_STAT(STAT_CyLandTessellatedComponents);
			}
		}

		if (NumSubsections > 1 && !CurrentLODData->UseCombinedMeshBatch)
		{
			INC_DWORD_STAT(STAT_CyLandComponentUsingSubSectionDrawCalls);

			for (int32 SubSectionIndex = 0; SubSectionIndex < MAX_SUBSECTION_COUNT; ++SubSectionIndex)
			{
				const FViewCustomDataSubSectionLOD& SubSectionLODData = CurrentLODData->SubSections[SubSectionIndex];
				check(SubSectionLODData.StaticBatchElementIndexToRender != INDEX_NONE);

				BatchesToRenderMask |= (((uint64)1) << SubSectionLODData.StaticBatchElementIndexToRender);
				INC_DWORD_STAT(STAT_CyLandDrawCalls);
				INC_DWORD_STAT_BY(STAT_CyLandTriangles, InBatch->Elements[SubSectionLODData.StaticBatchElementIndexToRender].NumPrimitives);
			}
		}
		else
		{
			int32 SubSectionIndex = 0;
			const FViewCustomDataSubSectionLOD& SubSectionLODData = CurrentLODData->SubSections[SubSectionIndex];
			check(SubSectionLODData.StaticBatchElementIndexToRender != INDEX_NONE);

			BatchesToRenderMask |= (((uint64)1) << SubSectionLODData.StaticBatchElementIndexToRender);
			INC_DWORD_STAT(STAT_CyLandDrawCalls);
			INC_DWORD_STAT_BY(STAT_CyLandTriangles, InBatch->Elements[SubSectionLODData.StaticBatchElementIndexToRender].NumPrimitives);
		}
	}

	INC_DWORD_STAT(STAT_CyLandComponentRenderPasses);

	return BatchesToRenderMask;
}

void FCyLandComponentSceneProxy::CalculateBatchElementLOD(const FSceneView& InView, float InMeshScreenSizeSquared, float InViewLODScale, FViewCustomDataLOD& InOutLODData, bool InForceCombined) const
{
	float SquaredViewLODScale = FMath::Square(InViewLODScale);

	check(InMeshScreenSizeSquared >= 0.0f && InMeshScreenSizeSquared <= 1.0f);
	float ComponentScreenSize = InMeshScreenSizeSquared;

	if (NumSubsections > 1)
	{
		InOutLODData.UseCombinedMeshBatch = false; // default to individual batch render

		float SubSectionMaxExtend = ComponentMaxExtend / 2.0f;
		float SubSectionRadius = CyLandComponent->Bounds.SphereRadius / 2.0f;
		float CombinedScreenRatio = 0.0f;
		bool AllSubSectionHaveSameScreenSize = true;

		// Compute screen size of each sub section to determine if we should use the combined logic or the individual logic
		for (int32 SubY = 0; SubY < NumSubsections; ++SubY)
		{
			for (int32 SubX = 0; SubX < NumSubsections; ++SubX)
			{
				int32 SubSectionIndex = SubX + SubY * NumSubsections;
				FViewCustomDataSubSectionLOD& SubSectionLODData = InOutLODData.SubSections[SubSectionIndex];

				SubSectionLODData.ScreenSizeSquared = GetComponentScreenSize(&InView, SubSectionScreenSizeTestingPosition[SubSectionIndex], SubSectionMaxExtend, SubSectionRadius);

				check(SubSectionLODData.ScreenSizeSquared > 0.0f);

				CalculateLODFromScreenSize(InView, SubSectionLODData.ScreenSizeSquared, InViewLODScale, SubSectionIndex, InOutLODData);
				check(SubSectionLODData.fBatchElementCurrentLOD != -1.0f);

				InOutLODData.ShaderCurrentLOD.Component(SubSectionIndex) = SubSectionLODData.fBatchElementCurrentLOD;

				// Determine if we should use the combined batch or not
				if (ComponentScreenSize > ComponentSquaredScreenSizeToUseSubSections * SquaredViewLODScale)
				{
					if (AllSubSectionHaveSameScreenSize)
					{
						float CurrentScreenRadiusSquared = SubSectionLODData.ScreenSizeSquared * SquaredViewLODScale;

						if (CombinedScreenRatio > 0.0f && !FMath::IsNearlyEqual(CombinedScreenRatio, CurrentScreenRadiusSquared, KINDA_SMALL_NUMBER))
						{
							AllSubSectionHaveSameScreenSize = false;
						}

						CombinedScreenRatio += CurrentScreenRadiusSquared;

						if (SubSectionIndex > 0)
						{
							CombinedScreenRatio *= 0.5f;
						}
					}
				}
			}
		}

		if (!GCyLandDebugOptions.IsCombinedDisabled() && (AllSubSectionHaveSameScreenSize || GCyLandDebugOptions.IsCombinedAll() || ForcedLOD != INDEX_NONE || InForceCombined))
		{
			InOutLODData.UseCombinedMeshBatch = true;

			int32 MinLOD = FMath::Min(InOutLODData.SubSections[0].BatchElementCurrentLOD, FMath::Min3(InOutLODData.SubSections[1].BatchElementCurrentLOD, InOutLODData.SubSections[2].BatchElementCurrentLOD, InOutLODData.SubSections[3].BatchElementCurrentLOD));

			for (int32 SubSectionIndex = 0; SubSectionIndex < MAX_SUBSECTION_COUNT; ++SubSectionIndex)
			{
				InOutLODData.SubSections[SubSectionIndex].BatchElementCurrentLOD = MinLOD;
			}
		}
	}
	else
	{
		const int32 SubSectionIndex = 0;
		InOutLODData.UseCombinedMeshBatch = true;
		FViewCustomDataSubSectionLOD& SubSectionLODData = InOutLODData.SubSections[SubSectionIndex];

		SubSectionLODData.ScreenSizeSquared = ComponentScreenSize;
		CalculateLODFromScreenSize(InView, SubSectionLODData.ScreenSizeSquared, InViewLODScale, SubSectionIndex, InOutLODData);
		check(SubSectionLODData.fBatchElementCurrentLOD != -1.0f);

		InOutLODData.ShaderCurrentLOD.Component(SubSectionIndex) = SubSectionLODData.fBatchElementCurrentLOD;
	}
}

int32 FCyLandComponentSceneProxy::ConvertBatchElementLODToBatchElementIndex(int8 BatchElementLOD, bool UseCombinedMeshBatch)
{
	int32 BatchElementIndex = BatchElementLOD;

	if (NumSubsections > 1 && ForcedLOD < 0)
	{
		BatchElementIndex = BatchElementLOD * (MAX_SUBSECTION_COUNT + 1);

		if (UseCombinedMeshBatch)
		{
			BatchElementIndex += MAX_SUBSECTION_COUNT;
		}
	}

	return BatchElementIndex;
}

float FCyLandComponentSceneProxy::ComputeBatchElementCurrentLOD(int32 InSelectedLODIndex, float InComponentScreenSize) const
{
	check(LODScreenRatioSquared.IsValidIndex(InSelectedLODIndex));

	bool LastElement = InSelectedLODIndex == LODScreenRatioSquared.Num() - 1;
	float CurrentLODScreenRatio = LODScreenRatioSquared[InSelectedLODIndex];
	float NextLODScreenRatio = LastElement ? 0 : LODScreenRatioSquared[InSelectedLODIndex + 1];

	float LODScreenRatioRange = CurrentLODScreenRatio - NextLODScreenRatio;

	if (InComponentScreenSize > CurrentLODScreenRatio || InComponentScreenSize < NextLODScreenRatio)
	{
		// Find corresponding LODIndex to appropriately calculate Ratio and apply it to new LODIndex
		int32 LODFromScreenSize = GetLODFromScreenSize(InComponentScreenSize, 1.0f); // for 4.19 only
		CurrentLODScreenRatio = LODScreenRatioSquared[LODFromScreenSize];
		NextLODScreenRatio = LODFromScreenSize == LODScreenRatioSquared.Num() - 1 ? 0 : LODScreenRatioSquared[LODFromScreenSize + 1];
		LODScreenRatioRange = CurrentLODScreenRatio - NextLODScreenRatio;
	}

	float CurrentLODRangeRatio = (InComponentScreenSize - NextLODScreenRatio) / LODScreenRatioRange;
	float fLOD = (float)InSelectedLODIndex + (1.0f - CurrentLODRangeRatio);

	return fLOD;
}

int8 FCyLandComponentSceneProxy::GetLODFromScreenSize(float InScreenSizeSquared, float InViewLODScale) const
{
	int32 LODScreenRatioSquaredCount = LODScreenRatioSquared.Num();
	float ScreenSizeSquared = InScreenSizeSquared / InViewLODScale;

	if (ScreenSizeSquared <= LODScreenRatioSquared[LODScreenRatioSquaredCount - 1])
	{
		return LODScreenRatioSquaredCount - 1;
	}
	else if (ScreenSizeSquared > LODScreenRatioSquared[1])
	{
		return 0;
	}
	else
	{
		int32 HalfPointIndex = (LODScreenRatioSquaredCount - 1) / 2;
		int32 StartingIndex = ScreenSizeSquared < LODScreenRatioSquared[HalfPointIndex] ? HalfPointIndex : 1;
		int8 SelectedLODIndex = INDEX_NONE;

		for (int32 i = StartingIndex; i < LODScreenRatioSquaredCount - 1; ++i)
		{
			if (ScreenSizeSquared > LODScreenRatioSquared[i + 1])
			{
				SelectedLODIndex = i;
				break;
			}
		}

		return SelectedLODIndex;
	}

	return INDEX_NONE;
}

void* FCyLandComponentSceneProxy::InitViewCustomData(const FSceneView& InView, float InViewLODScale, FMemStackBase& InCustomDataMemStack, bool InIsStaticRelevant, bool InIsShadowOnly, const FLODMask* InVisiblePrimitiveLODMask, float InMeshScreenSizeSquared)
{
	SCOPE_CYCLE_COUNTER(STAT_CyLandInitViewCustomData);

	// NOTE: we can't access other proxy here as this can be run in parallel we need to wait for the PostInitViewCustomData which is run in synchronous	

	PrimitiveCustomDataIndex = GetPrimitiveSceneInfo()->GetIndex();

	FViewCustomDataLOD* LODData = (FViewCustomDataLOD*)new(InCustomDataMemStack) FViewCustomDataLOD();

	check(InMeshScreenSizeSquared <= 1.0f);
	LODData->ComponentScreenSize = InMeshScreenSizeSquared;

	// If a valid screen size was provided, we use it instead of recomputing it
	if (InMeshScreenSizeSquared < 0.0f)
	{
		LODData->ComponentScreenSize = GetComponentScreenSize(&InView, CyLandComponent->Bounds.Origin, ComponentMaxExtend, CyLandComponent->Bounds.SphereRadius);
	}

	CalculateBatchElementLOD(InView, LODData->ComponentScreenSize, InViewLODScale, *LODData, false);

	if (InIsStaticRelevant)
	{
		check(InVisiblePrimitiveLODMask != nullptr);
		LODData->StaticMeshBatchLOD = InVisiblePrimitiveLODMask->DitheredLODIndices[0];

		if (LODData->UseCombinedMeshBatch)
		{
			ComputeStaticBatchIndexToRender(*LODData, 0);
		}
		else
		{
			for (int32 i = 0; i < MAX_SUBSECTION_COUNT; ++i)
			{
				ComputeStaticBatchIndexToRender(*LODData, i);
			}
		}
	}

	LODData->IsShadowOnly = InIsShadowOnly;

	// Mobile use a different way of calculating the Bias
	if (GetScene().GetFeatureLevel() >= ERHIFeatureLevel::SM4)
	{
		LODData->LodBias = GetShaderLODBias();
	}

	ComputeTessellationFalloffShaderValues(*LODData, InView.ViewMatrices.GetProjectionMatrix(), LODData->LodTessellationParams.X, LODData->LodTessellationParams.Y);

	return LODData;
}

void FCyLandComponentSceneProxy::ComputeTessellationFalloffShaderValues(const FViewCustomDataLOD& InLODData, const FMatrix& InViewProjectionMatrix, float& OutC, float& OutK) const
{
	// No Falloff
	OutC = 1.0f;
	OutK = 0.0f;

	const auto FeatureLevel = GetScene().GetFeatureLevel();
	bool HasTessellationEnabled = false;

	if (FeatureLevel >= ERHIFeatureLevel::SM4)
	{
		const int8 CurrentLODIndex = InLODData.SubSections[0].BatchElementCurrentLOD;
		UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

		if (CyLandMIC == nullptr)
		{
			UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

			if (CyLandMID != nullptr)
			{
				CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
			}
		}

		HasTessellationEnabled = MaterialHasTessellationEnabled[LODIndexToMaterialIndex[CurrentLODIndex]] && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation);
	}

	if (HasTessellationEnabled && (InLODData.StaticMeshBatchLOD == INDEX_NONE || (InLODData.StaticMeshBatchLOD == 0 || InLODData.StaticMeshBatchLOD == 2))) // Tess batch will be used
	{
		if (UseTessellationComponentScreenSizeFalloff)
		{
			float MaxTesselationDistance = ComputeBoundsDrawDistance(FMath::Sqrt(TessellationComponentSquaredScreenSize), CyLandComponent->Bounds.SphereRadius / 2.0f, InViewProjectionMatrix);
			float FallOffStartingDistance = FMath::Min(ComputeBoundsDrawDistance(FMath::Sqrt(FMath::Min(FMath::Square(TessellationComponentScreenSizeFalloff), TessellationComponentSquaredScreenSize)), CyLandComponent->Bounds.SphereRadius / 2.0f, InViewProjectionMatrix) - MaxTesselationDistance, MaxTesselationDistance);

			// Calculate the falloff using a = C - K * d by sending C & K into the shader
			OutC = MaxTesselationDistance / (MaxTesselationDistance - FallOffStartingDistance);
			OutK = -(1 / (-MaxTesselationDistance + FallOffStartingDistance));
		}
	}
}

FVector4 FCyLandComponentSceneProxy::GetShaderLODBias() const
{
	return FVector4(
		0.0f, // unused
		0.0f, // unused
		((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip(),
		XYOffsetmapTexture ? ((FTexture2DResource*)XYOffsetmapTexture->Resource)->GetCurrentFirstMip() : 0.0f);
}

FVector4 FCyLandComponentSceneProxy::GetShaderLODValues(int8 InBatchElementCurrentLOD) const
{
	return FVector4(
		InBatchElementCurrentLOD,
		0.0f, // unused
		(float)((SubsectionSizeVerts >> InBatchElementCurrentLOD) - 1),
		1.f / (float)((SubsectionSizeVerts >> InBatchElementCurrentLOD) - 1));
}

void FCyLandComponentSceneProxy::GetShaderCurrentNeighborLOD(const FSceneView& InView, float InBatchElementCurrentLOD, int8 InSubSectionX, int8 InSubSectionY, int8 InCurrentSubSectionIndex, FVector4& OutShaderCurrentNeighborLOD) const
{
	for (int32 NeighborIndex = 0; NeighborIndex < NEIGHBOR_COUNT; ++NeighborIndex)
	{
		OutShaderCurrentNeighborLOD.Component(NeighborIndex) = GetNeighborLOD(InView, InBatchElementCurrentLOD, NeighborIndex, InSubSectionX, InSubSectionY, InCurrentSubSectionIndex);
		check(OutShaderCurrentNeighborLOD.Component(NeighborIndex) != -1.0f);
	}
}

struct SubSectionData
{
	SubSectionData(int8 InSubSectionOffsetX, int8 InSubSectionOffsetY, bool InInsideComponent)
		: SubSectionOffsetX(InSubSectionOffsetX)
		, SubSectionOffsetY(InSubSectionOffsetY)
		, InsideComponent(InInsideComponent)
	{}

	int8 SubSectionOffsetX;
	int8 SubSectionOffsetY;
	bool InsideComponent;
};

// SubSectionIndex, NeighborIndex
static const SubSectionData SubSectionValues[4][4] = { { SubSectionData(0, 1, false), SubSectionData(1, 0, false), SubSectionData(1, 0, true), SubSectionData(0, 1, true) },
{ SubSectionData(0, 1, false), SubSectionData(-1, 0, true), SubSectionData(-1, 0, false), SubSectionData(0, 1, true) },
{ SubSectionData(0, -1, true), SubSectionData(1, 0, false), SubSectionData(1, 0, true), SubSectionData(0, -1, false) },
{ SubSectionData(0, -1, true), SubSectionData(-1, 0, true), SubSectionData(-1, 0, false), SubSectionData(0, -1, false) } };


float FCyLandComponentSceneProxy::GetNeighborLOD(const FSceneView& InView, float InBatchElementCurrentLOD, int8 InNeighborIndex, int8 InSubSectionX, int8 InSubSectionY, int8 InCurrentSubSectionIndex) const
{
	float NeighborLOD = InBatchElementCurrentLOD;

	// Assume no sub section initialization
	bool InsideComponent = false;
	int32 PrimitiveDataIndex = PrimitiveCustomDataIndex;
	int32 DesiredSubSectionIndex = 0;
	int32 DesiredSubSectionX = 0;
	int32 DesiredSubSectionY = 0;
	int32 CurrentSubSectionIndex = InSubSectionX != INDEX_NONE && InSubSectionY != INDEX_NONE ? InSubSectionX + InSubSectionY * NumSubsections : 0;

	// Handle subsection
	if (InSubSectionX != INDEX_NONE && InSubSectionY != INDEX_NONE)
	{

		const SubSectionData& Data = SubSectionValues[CurrentSubSectionIndex][InNeighborIndex];
		DesiredSubSectionX = InSubSectionX + Data.SubSectionOffsetX;
		DesiredSubSectionY = InSubSectionY + Data.SubSectionOffsetY;
		DesiredSubSectionIndex = DesiredSubSectionX + DesiredSubSectionY * NumSubsections;
		InsideComponent = Data.InsideComponent;
	}

	const FCyLandNeighborInfo* Neighbor = nullptr;

	if (!InsideComponent)
	{
		Neighbor = Neighbors[InNeighborIndex];

		if (Neighbor != nullptr)
		{
			PrimitiveDataIndex = Neighbor->PrimitiveCustomDataIndex;
		}
		else
		{
			DesiredSubSectionIndex = CurrentSubSectionIndex;
		}
	}

	bool ComputeNeighborCustomDataLOD = true;

	void* CustomData = InView.GetCustomData(PrimitiveDataIndex);

	if (CustomData != nullptr)
	{
		const FViewCustomDataLOD* LODData = (const FViewCustomDataLOD*)CustomData;
		// Don't use the custom data for neighbor calculation when it is marked shadow only (ie it is not visible in the view)
		// See UE-69785 for more information
		if (!LODData->IsShadowOnly)
		{
			ComputeNeighborCustomDataLOD = false;
			NeighborLOD = FMath::Max(LODData->SubSections[DesiredSubSectionIndex].fBatchElementCurrentLOD, InBatchElementCurrentLOD);
		}
	}

	if (ComputeNeighborCustomDataLOD)
	{
		FVector CyLandComponentOrigin = CyLandComponent->Bounds.Origin;
		float CyLandComponentMaxExtends = ComponentMaxExtend;

		if (Neighbor != nullptr)
		{
			const UCyLandComponent* NeighborComponent = Neighbor->GetCyLandComponent();

			if (Neighbor->GetCyLandComponent() != nullptr)
			{
				CyLandComponentOrigin = NeighborComponent->Bounds.Origin;
				CyLandComponentMaxExtends = NeighborComponent->SubsectionSizeQuads * FMath::Max(NeighborComponent->GetComponentTransform().GetScale3D().X, NeighborComponent->GetComponentTransform().GetScale3D().Y);
			}
		}

		if (NumSubsections > 1)
		{
			float SubSectionMaxExtend = CyLandComponentMaxExtends / 2.0f;
			FVector ComponentTopLeftCorner = CyLandComponentOrigin - FVector(SubSectionMaxExtend, SubSectionMaxExtend, 0.0f);

			FVector SubSectionOrigin = ComponentTopLeftCorner + FVector(CyLandComponentMaxExtends * DesiredSubSectionX, CyLandComponentMaxExtends * DesiredSubSectionY, 0.0f);
			float MeshBatchScreenSizeSquared = GetComponentScreenSize(&InView, SubSectionOrigin, SubSectionMaxExtend, CyLandComponent->Bounds.SphereRadius / 2.0f);

			FViewCustomDataLOD NeighborLODData;
			CalculateLODFromScreenSize(InView, MeshBatchScreenSizeSquared, InView.LODDistanceFactor, DesiredSubSectionIndex, NeighborLODData);
			const FViewCustomDataSubSectionLOD& SubSectionData = NeighborLODData.SubSections[DesiredSubSectionIndex];
			check(SubSectionData.fBatchElementCurrentLOD != -1.0f);

			if (SubSectionData.fBatchElementCurrentLOD > InBatchElementCurrentLOD)
			{
				NeighborLOD = SubSectionData.fBatchElementCurrentLOD;
			}
		}
		else
		{
			float MeshBatchScreenSizeSquared = GetComponentScreenSize(&InView, CyLandComponentOrigin, CyLandComponentMaxExtends, CyLandComponent->Bounds.SphereRadius);

			FViewCustomDataLOD NeighborLODData;
			CalculateLODFromScreenSize(InView, MeshBatchScreenSizeSquared, InView.LODDistanceFactor, 0, NeighborLODData);

			FViewCustomDataSubSectionLOD& SubSectionLODData = NeighborLODData.SubSections[0];
			check(SubSectionLODData.fBatchElementCurrentLOD != -1.0f);

			if (SubSectionLODData.fBatchElementCurrentLOD > InBatchElementCurrentLOD)
			{
				NeighborLOD = SubSectionLODData.fBatchElementCurrentLOD;
			}
		}
	}

	return NeighborLOD;
}

void FCyLandComponentSceneProxy::ComputeStaticBatchIndexToRender(FViewCustomDataLOD& OutLODData, int32 SubSectionIndex)
{
	FViewCustomDataSubSectionLOD& SubSectionLODData = OutLODData.SubSections[SubSectionIndex];

	SubSectionLODData.StaticBatchElementIndexToRender = INDEX_NONE;
	SubSectionLODData.StaticBatchElementIndexToRender = ConvertBatchElementLODToBatchElementIndex(SubSectionLODData.BatchElementCurrentLOD, OutLODData.UseCombinedMeshBatch) + SubSectionIndex;
	check(SubSectionLODData.StaticBatchElementIndexToRender != INDEX_NONE);
}

void FCyLandComponentSceneProxy::PostInitViewCustomData(const FSceneView& InView, void* InViewCustomData) const
{
	SCOPE_CYCLE_COUNTER(STAT_CyLandPostInitViewCustomData);

	FViewCustomDataLOD* CurrentLODData = (FViewCustomDataLOD*)InViewCustomData;
	check(CurrentLODData != nullptr);

	for (int8 SubY = 0; SubY < NumSubsections; ++SubY)
	{
		for (int8 SubX = 0; SubX < NumSubsections; ++SubX)
		{
			int8 SubSectionIndex = SubX + SubY * NumSubsections;
			FViewCustomDataSubSectionLOD& SubSectionLODData = CurrentLODData->SubSections[SubSectionIndex];
			GetShaderCurrentNeighborLOD(InView, SubSectionLODData.fBatchElementCurrentLOD, NumSubsections > 1 ? SubX : INDEX_NONE, NumSubsections > 1 ? SubY : INDEX_NONE, SubSectionIndex, SubSectionLODData.ShaderCurrentNeighborLOD);
		}
	}

#if !UE_BUILD_SHIPPING
	if (GVarDumpCyLandLODs && GFrameNumberRenderThread == GVarDumpCyLandLODsCurrentFrame)
	{
		if (NumSubsections == 1)
		{
			UE_LOG(LogCyLand, Warning, TEXT("\nComponent: [%s] -> MeshBatchLOD: %d, ComponentScreenSize: %f, ShaderCurrentLOD: %s, LODTessellation: %s\nHeightmap Texture Name: %s, Heightmap Streamed Mip: %d\nSubSections:\n|0| IndexToRender: %d, fLOD: %f, LOD: %d, NeighborLOD: %s\n"), *SectionBase.ToString(),
				CurrentLODData->StaticMeshBatchLOD, CurrentLODData->ComponentScreenSize, *CurrentLODData->ShaderCurrentLOD.ToString(), *CurrentLODData->LodTessellationParams.ToString(),
				HeightmapTexture != nullptr ? *HeightmapTexture->GetFullName() : TEXT("Invalid"), HeightmapTexture != nullptr ? ((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip() : INDEX_NONE,
				CurrentLODData->SubSections[0].StaticBatchElementIndexToRender, CurrentLODData->SubSections[0].fBatchElementCurrentLOD, CurrentLODData->SubSections[0].BatchElementCurrentLOD, *CurrentLODData->SubSections[0].ShaderCurrentNeighborLOD.ToString());
		}
		else
		{
			UE_LOG(LogCyLand, Warning, TEXT("\nComponent: [%s] -> MeshBatchLOD: %d, ComponentScreenSize: %f, UseCombinedMeshBatch: %d, ShaderCurrentLOD: %s, LODTessellation: %s\nHeightmap Texture Name: %s, Heightmap Streamed Mip: %d\nSubSections:\n|0| IndexToRender: %d, fLOD: %f, LOD: %d, NeighborLOD: %s\n|1| IndexToRender: %d, fLOD: %f, LOD: %d, NeighborLOD: %s\n|2| IndexToRender: %d, fLOD: %f, LOD: %d, NeighborLOD: %s\n|3| IndexToRender: %d, fLOD: %f, LOD: %d, NeighborLOD: %s\n"),
				*SectionBase.ToString(), CurrentLODData->StaticMeshBatchLOD, CurrentLODData->ComponentScreenSize, CurrentLODData->UseCombinedMeshBatch, *CurrentLODData->ShaderCurrentLOD.ToString(), *CurrentLODData->LodTessellationParams.ToString(),
				HeightmapTexture != nullptr ? *HeightmapTexture->GetFullName() : TEXT("Invalid"), HeightmapTexture != nullptr ? ((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip() : INDEX_NONE,
				CurrentLODData->SubSections[0].StaticBatchElementIndexToRender, CurrentLODData->SubSections[0].fBatchElementCurrentLOD, CurrentLODData->SubSections[0].BatchElementCurrentLOD, *CurrentLODData->SubSections[0].ShaderCurrentNeighborLOD.ToString(),
				CurrentLODData->SubSections[1].StaticBatchElementIndexToRender, CurrentLODData->SubSections[1].fBatchElementCurrentLOD, CurrentLODData->SubSections[1].BatchElementCurrentLOD, *CurrentLODData->SubSections[1].ShaderCurrentNeighborLOD.ToString(),
				CurrentLODData->SubSections[2].StaticBatchElementIndexToRender, CurrentLODData->SubSections[2].fBatchElementCurrentLOD, CurrentLODData->SubSections[2].BatchElementCurrentLOD, *CurrentLODData->SubSections[2].ShaderCurrentNeighborLOD.ToString(),
				CurrentLODData->SubSections[3].StaticBatchElementIndexToRender, CurrentLODData->SubSections[3].fBatchElementCurrentLOD, CurrentLODData->SubSections[3].BatchElementCurrentLOD, *CurrentLODData->SubSections[3].ShaderCurrentNeighborLOD.ToString());
		}
	}
#endif
}

bool FCyLandComponentSceneProxy::IsUsingCustomLODRules() const
{
	return true;
}

bool FCyLandComponentSceneProxy::IsUsingCustomWholeSceneShadowLODRules() const
{
	return true;
}

bool FCyLandComponentSceneProxy::CanUseMeshBatchForShadowCascade(int8 InLODIndex, float InShadowMapTextureResolution, float InShadowMapCascadeSize) const
{
	const FStaticMeshBatch* MeshBatch = nullptr;
	const TArray<FStaticMeshBatch>& PrimitiveStaticMeshes = GetPrimitiveSceneInfo()->StaticMeshes;

	check(PrimitiveStaticMeshes.IsValidIndex(InLODIndex));
	check(PrimitiveStaticMeshes[InLODIndex].CastShadow);
	check(InLODIndex == PrimitiveStaticMeshes[InLODIndex].LODIndex);
	MeshBatch = &PrimitiveStaticMeshes[InLODIndex];

	check(MeshBatch != nullptr);

	if (!MeshBatch->CastShadow || MeshBatch->TessellationDisablingShadowMapMeshSize == 0)
	{
		return true;
	}

	float WorldUnitsForOneTexel = InShadowMapCascadeSize / InShadowMapTextureResolution; // We assume Shadow Map texture to be squared
	return MeshBatch->TessellationDisablingShadowMapMeshSize >= WorldUnitsForOneTexel * (GShadowMapWorldUnitsToTexelFactor != -1.0f ? GShadowMapWorldUnitsToTexelFactor : 1.0f);
}

FLODMask FCyLandComponentSceneProxy::GetCustomLOD(const FSceneView& InView, float InViewLODScale, int32 InForcedLODLevel, float& OutScreenSizeSquared) const
{
	SCOPE_CYCLE_COUNTER(STAT_CyLandComputeCustomMeshBatchLOD);
	FLODMask LODToRender;
	OutScreenSizeSquared = 0.0f;

	// Handle forced LOD level first
	if (InForcedLODLevel >= 0)
	{
		int8 MinMeshLOD = MAX_int8;
		int8 MaxMeshLOD = 0;
		GetPrimitiveSceneInfo()->GetStaticMeshesLODRange(MinMeshLOD, MaxMeshLOD);

		LODToRender.SetLOD(FMath::Clamp<int8>(InForcedLODLevel, MinMeshLOD, MaxMeshLOD));
	}
	else if (InView.Family->EngineShowFlags.LOD)
	{
		int8 PotentialLOD = INDEX_NONE;
		OutScreenSizeSquared = GetComponentScreenSize(&InView, CyLandComponent->Bounds.Origin, ComponentMaxExtend, CyLandComponent->Bounds.SphereRadius);

		if (NumSubsections > 1)
		{
			float SubSectionMaxExtend = ComponentMaxExtend / 2.0f;
			float SubSectionRadius = CyLandComponent->Bounds.SphereRadius / 2.0f;

			// Compute screen size of each sub section to determine if we should use the combined logic or the individual logic
			float ScreenSizeSquared = GetComponentScreenSize(&InView, SubSectionScreenSizeTestingPosition[0], SubSectionMaxExtend, SubSectionRadius);
			PotentialLOD = GetLODFromScreenSize(ScreenSizeSquared, InViewLODScale);
		}
		else
		{
			PotentialLOD = GetLODFromScreenSize(OutScreenSizeSquared, InViewLODScale);
		}

		const auto FeatureLevel = InView.GetFeatureLevel();
		bool HasTessellationEnabled = false;

		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[LODIndexToMaterialIndex[PotentialLOD]]);

			if (CyLandMIC == nullptr)
			{
				UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[LODIndexToMaterialIndex[PotentialLOD]]);

				if (CyLandMID != nullptr)
				{
					CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
				}
			}

			HasTessellationEnabled = MaterialHasTessellationEnabled[LODIndexToMaterialIndex[PotentialLOD]] && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation);
		}

		int32 BaseMeshBatchIndex = MaterialIndexToStaticMeshBatchLOD[LODIndexToMaterialIndex[PotentialLOD]];

		if (HasTessellationEnabled)
		{
			const int8 TessellatedMeshBatchLODIndex = 0;
			const int8 NonTessellatedMeshBatchLODIndex = 1;
			LODToRender.SetLOD(OutScreenSizeSquared >= TessellationComponentSquaredScreenSize * InViewLODScale ? BaseMeshBatchIndex + TessellatedMeshBatchLODIndex : BaseMeshBatchIndex + NonTessellatedMeshBatchLODIndex);
		}
		else
		{
			LODToRender.SetLOD(BaseMeshBatchIndex);
		}
	}

	return LODToRender;
}

FLODMask FCyLandComponentSceneProxy::GetCustomWholeSceneShadowLOD(const FSceneView& InView, float InViewLODScale, int32 InForcedLODLevel, const FLODMask& InVisibilePrimitiveLODMask, float InShadowMapTextureResolution, float InShadowMapCascadeSize, int8 InShadowCascadeId, bool InHasSelfShadow) const
{
	SCOPE_CYCLE_COUNTER(STAT_CyLandComputeCustomShadowMeshBatchLOD);

	FLODMask LODToRender;

	// Handle forced LOD level first
	if (InForcedLODLevel >= 0)
	{
		int8 MinMeshLOD = MAX_int8;
		int8 MaxMeshLOD = 0;
		GetPrimitiveSceneInfo()->GetStaticMeshesLODRange(MinMeshLOD, MaxMeshLOD);

		LODToRender.SetLOD(FMath::Clamp<int8>(InForcedLODLevel, MinMeshLOD, MaxMeshLOD));
	}
	else if (!InHasSelfShadow) // Force lowest valid LOD
	{
		int8 MinMeshLOD = MAX_int8;
		int8 MaxMeshLOD = 0;
		GetPrimitiveSceneInfo()->GetStaticMeshesLODRange(MinMeshLOD, MaxMeshLOD);
		LODToRender.SetLOD(MinMeshLOD);
	}
	else
	{
		const FViewCustomDataLOD* PrimitiveCustomData = (const FViewCustomDataLOD*)InView.GetCustomData(GetPrimitiveSceneInfo()->GetIndex());
		int8 PotentialLOD = INDEX_NONE;
		float ScreenSizeSquared = 0.0f;

		if (PrimitiveCustomData == nullptr)
		{
			ScreenSizeSquared = GetComponentScreenSize(&InView, CyLandComponent->Bounds.Origin, ComponentMaxExtend, CyLandComponent->Bounds.SphereRadius);

			if (NumSubsections > 1)
			{
				float SubSectionMaxExtend = ComponentMaxExtend / 2.0f;
				float SubSectionRadius = CyLandComponent->Bounds.SphereRadius / 2.0f;

				// Compute screen size of each sub section to determine if we should use the combined logic or the individual logic
				float SubSectionScreenSizeSquared = GetComponentScreenSize(&InView, SubSectionScreenSizeTestingPosition[0], SubSectionMaxExtend, SubSectionRadius);
				PotentialLOD = GetLODFromScreenSize(SubSectionScreenSizeSquared, InViewLODScale);
			}
			else
			{
				PotentialLOD = GetLODFromScreenSize(ScreenSizeSquared, InViewLODScale);
			}
		}
		else
		{
			ScreenSizeSquared = PrimitiveCustomData->ComponentScreenSize;
			PotentialLOD = PrimitiveCustomData->SubSections[0].BatchElementCurrentLOD;
		}

		const auto FeatureLevel = InView.GetFeatureLevel();
		bool HasTessellationEnabled = false;

		if (FeatureLevel >= ERHIFeatureLevel::SM4)
		{
			UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[LODIndexToMaterialIndex[PotentialLOD]]);

			if (CyLandMIC == nullptr)
			{
				UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[LODIndexToMaterialIndex[PotentialLOD]]);

				if (CyLandMID != nullptr)
				{
					CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
				}
			}

			HasTessellationEnabled = MaterialHasTessellationEnabled[LODIndexToMaterialIndex[PotentialLOD]] && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation);
		}

		int32 BaseMeshBatchIndex = MaterialIndexToStaticMeshBatchLOD[LODIndexToMaterialIndex[PotentialLOD]];

		if (HasTessellationEnabled)
		{
			const int8 ShadowTessellatedMeshBatchLODIndex = 2;
			const int8 ShadowNonTessellatedMeshBatchLODIndex = 3;

			bool UseTessellationMeshBatch = ScreenSizeSquared >= TessellationComponentSquaredScreenSize * InViewLODScale;

			if (UseTessellationMeshBatch)
			{
				if (!CanUseMeshBatchForShadowCascade(ShadowTessellatedMeshBatchLODIndex, InShadowMapTextureResolution, InShadowMapCascadeSize))
				{
					UseTessellationMeshBatch = false;
				}
			}

			LODToRender.SetLOD(UseTessellationMeshBatch ? BaseMeshBatchIndex + ShadowTessellatedMeshBatchLODIndex : BaseMeshBatchIndex + ShadowNonTessellatedMeshBatchLODIndex);
		}
		else
		{
			LODToRender.SetLOD(BaseMeshBatchIndex);
		}
	}

	return LODToRender;
}

namespace
{
	FLinearColor GetColorForLod(int32 CurrentLOD, int32 ForcedLOD, bool DisplayCombinedBatch)
	{
		int32 ColorIndex = INDEX_NONE;
		if (GEngine->LODColorationColors.Num() > 0)
		{
			ColorIndex = CurrentLOD;
			ColorIndex = FMath::Clamp(ColorIndex, 0, GEngine->LODColorationColors.Num() - 1);
		}
		const FLinearColor& LODColor = ColorIndex != INDEX_NONE ? GEngine->LODColorationColors[ColorIndex] : FLinearColor::Gray;

		if (ForcedLOD >= 0)
		{
			return LODColor;
		}

		if (DisplayCombinedBatch)
		{
			return LODColor * 0.2f;
		}

		return LODColor * 0.1f;
	}
}

void FCyLandComponentSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	//UE_LOG(LogCyLand, Warning, TEXT("ACyLand GetDynamicMeshElements GetDynamicMeshElements"));
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FCyLandComponentSceneProxy_GetMeshElements);
	SCOPE_CYCLE_COUNTER(STAT_CyLandDynamicDrawTime);

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const bool bInCollisionView = ViewFamily.EngineShowFlags.CollisionVisibility || ViewFamily.EngineShowFlags.CollisionPawn;
	const bool bDrawSimpleCollision = ViewFamily.EngineShowFlags.CollisionPawn       && CollisionResponse.GetResponse(ECC_Pawn) != ECR_Ignore;
	const bool bDrawComplexCollision = ViewFamily.EngineShowFlags.CollisionVisibility && CollisionResponse.GetResponse(ECC_Visibility) != ECR_Ignore;
#endif

	int32 NumPasses = 0;
	int32 NumTriangles = 0;
	int32 NumDrawCalls = 0;
	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];
			FCyLandElementParamArray& ParameterArray = Collector.AllocateOneFrameResource<FCyLandElementParamArray>();

			const FViewCustomDataLOD* PrimitiveCustomData = (const FViewCustomDataLOD*)View->GetCustomData(GetPrimitiveSceneInfo()->GetIndex());

			if (PrimitiveCustomData == nullptr)
			{
				continue;
			}

			ParameterArray.ElementParams.AddDefaulted(NumSubsections * NumSubsections);

			const auto FeatureLevel = View->GetFeatureLevel();
			bool HasTessellationEnabled = false;

			if (FeatureLevel >= ERHIFeatureLevel::SM4)
			{
				const int8 CurrentLODIndex = PrimitiveCustomData->SubSections[0].BatchElementCurrentLOD;
				UCyLandMaterialInstanceConstant* CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

				if (CyLandMIC == nullptr)
				{
					UMaterialInstanceDynamic* CyLandMID = Cast<UMaterialInstanceDynamic>(AvailableMaterials[LODIndexToMaterialIndex[CurrentLODIndex]]);

					if (CyLandMID != nullptr)
					{
						CyLandMIC = Cast<UCyLandMaterialInstanceConstant>(CyLandMID->Parent);
					}
				}

				HasTessellationEnabled = MaterialHasTessellationEnabled[LODIndexToMaterialIndex[CurrentLODIndex]] && (CyLandMIC != nullptr && !CyLandMIC->bDisableTessellation) && AvailableMaterials.Num() > 1;
			}

			bool DisableTessellation = false;

			if (HasTessellationEnabled)
			{
				check(AvailableMaterials.Num() > 1);
				float ScreenSizeSquared = GetComponentScreenSize(View, CyLandComponent->Bounds.Origin, ComponentMaxExtend, CyLandComponent->Bounds.SphereRadius);
				DisableTessellation = (ScreenSizeSquared < TessellationComponentSquaredScreenSize * View->LODDistanceFactor) ? true : false;

				if (!DisableTessellation)
				{
					INC_DWORD_STAT(STAT_CyLandTessellatedComponents);
				}
			}

			if (!PrimitiveCustomData->UseCombinedMeshBatch)
			{
				INC_DWORD_STAT(STAT_CyLandComponentUsingSubSectionDrawCalls);
			}

			FMeshBatch& Mesh = Collector.AllocateMesh();
			BuildDynamicMeshElement(PrimitiveCustomData, false, HasTessellationEnabled, DisableTessellation, Mesh, ParameterArray.ElementParams);

#if WITH_EDITOR
			FMeshBatch& MeshTools = Collector.AllocateMesh();

			// No Tessellation on tool material
			BuildDynamicMeshElement(PrimitiveCustomData, true, HasTessellationEnabled, true, MeshTools, ParameterArray.ElementParams);
#endif

			// Render the landscape component
#if WITH_EDITOR
			switch (GCyLandViewMode)
			{
			case ECyLandViewMode::DebugLayer:
			{
				if (GLayerDebugColorMaterial)
				{
					auto DebugColorMaterialInstance = new FCyLandDebugMaterialRenderProxy(GLayerDebugColorMaterial->GetRenderProxy(),
						(EditToolRenderData.DebugChannelR >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelR / 4] : nullptr),
						(EditToolRenderData.DebugChannelG >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelG / 4] : nullptr),
						(EditToolRenderData.DebugChannelB >= 0 ? WeightmapTextures[EditToolRenderData.DebugChannelB / 4] : nullptr),
						(EditToolRenderData.DebugChannelR >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelR % 4] : DebugColorMask::Masks[4]),
						(EditToolRenderData.DebugChannelG >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelG % 4] : DebugColorMask::Masks[4]),
						(EditToolRenderData.DebugChannelB >= 0 ? DebugColorMask::Masks[EditToolRenderData.DebugChannelB % 4] : DebugColorMask::Masks[4])
					);

					MeshTools.MaterialRenderProxy = DebugColorMaterialInstance;
					Collector.RegisterOneFrameMaterialProxy(DebugColorMaterialInstance);

					MeshTools.bCanApplyViewModeOverrides = true;
					MeshTools.bUseWireframeSelectionColoring = IsSelected();

					Collector.AddMesh(ViewIndex, MeshTools);

					NumPasses++;
					NumTriangles += MeshTools.GetNumPrimitives();
					NumDrawCalls += MeshTools.Elements.Num();
				}
			}
			break;

			case ECyLandViewMode::LayerDensity:
			{
				int32 ColorIndex = FMath::Min<int32>(NumWeightmapLayerAllocations, GEngine->ShaderComplexityColors.Num());
				auto LayerDensityMaterialInstance = new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), ColorIndex ? GEngine->ShaderComplexityColors[ColorIndex - 1] : FLinearColor::Black);

				MeshTools.MaterialRenderProxy = LayerDensityMaterialInstance;
				Collector.RegisterOneFrameMaterialProxy(LayerDensityMaterialInstance);

				MeshTools.bCanApplyViewModeOverrides = true;
				MeshTools.bUseWireframeSelectionColoring = IsSelected();

				Collector.AddMesh(ViewIndex, MeshTools);

				NumPasses++;
				NumTriangles += MeshTools.GetNumPrimitives();
				NumDrawCalls += MeshTools.Elements.Num();
			}
			break;

			case ECyLandViewMode::LayerUsage:
			{
				if (GCyLandLayerUsageMaterial)
				{
					float Rotation = ((SectionBase.X / ComponentSizeQuads) ^ (SectionBase.Y / ComponentSizeQuads)) & 1 ? 0 : 2.f * PI;
					auto LayerUsageMaterialInstance = new FCyLandLayerUsageRenderProxy(GCyLandLayerUsageMaterial->GetRenderProxy(), ComponentSizeVerts, LayerColors, Rotation);
					MeshTools.MaterialRenderProxy = LayerUsageMaterialInstance;
					Collector.RegisterOneFrameMaterialProxy(LayerUsageMaterialInstance);
					MeshTools.bCanApplyViewModeOverrides = true;
					MeshTools.bUseWireframeSelectionColoring = IsSelected();
					Collector.AddMesh(ViewIndex, MeshTools);
					NumPasses++;
					NumTriangles += MeshTools.GetNumPrimitives();
					NumDrawCalls += MeshTools.Elements.Num();
				}
			}
			break;

			case ECyLandViewMode::LOD:
			{

				const bool bMaterialModifiesMeshPosition = Mesh.MaterialRenderProxy->GetMaterial(View->GetFeatureLevel())->MaterialModifiesMeshPosition_RenderThread();

				auto& TemplateMesh = bIsWireframe ? Mesh : MeshTools;
				for (int32 i = 0; i < TemplateMesh.Elements.Num(); i++)
				{
					FMeshBatch& LODMesh = Collector.AllocateMesh();
					LODMesh = TemplateMesh;
					LODMesh.Elements.Empty(1);
					LODMesh.Elements.Add(TemplateMesh.Elements[i]);
					int32 CurrentLOD = ((FCyLandBatchElementParams*)TemplateMesh.Elements[i].UserData)->CurrentLOD;
					LODMesh.VisualizeLODIndex = CurrentLOD;
					FLinearColor Color = GetColorForLod(CurrentLOD, ForcedLOD, PrimitiveCustomData != nullptr ? PrimitiveCustomData->UseCombinedMeshBatch : true);
					FMaterialRenderProxy* LODMaterialProxy = (FMaterialRenderProxy*)new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), Color);
					Collector.RegisterOneFrameMaterialProxy(LODMaterialProxy);
					LODMesh.MaterialRenderProxy = LODMaterialProxy;
					LODMesh.bCanApplyViewModeOverrides = !bIsWireframe;
					LODMesh.bWireframe = bIsWireframe;
					LODMesh.bUseWireframeSelectionColoring = IsSelected();
					Collector.AddMesh(ViewIndex, LODMesh);

					NumTriangles += TemplateMesh.Elements[i].NumPrimitives;
					NumDrawCalls++;
				}
				NumPasses++;

			}
			break;

			case ECyLandViewMode::WireframeOnTop:
			{
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
				NumPasses++;
				NumTriangles += Mesh.GetNumPrimitives();
				NumDrawCalls += Mesh.Elements.Num();

				// wireframe on top
				FMeshBatch& WireMesh = Collector.AllocateMesh();
				WireMesh = MeshTools;
				auto WireMaterialInstance = new FColoredMaterialRenderProxy(GEngine->LevelColorationUnlitMaterial->GetRenderProxy(), FLinearColor(0, 0, 1));
				WireMesh.MaterialRenderProxy = WireMaterialInstance;
				Collector.RegisterOneFrameMaterialProxy(WireMaterialInstance);
				WireMesh.bCanApplyViewModeOverrides = false;
				WireMesh.bWireframe = true;
				Collector.AddMesh(ViewIndex, WireMesh);
				NumPasses++;
				NumTriangles += WireMesh.GetNumPrimitives();
				NumDrawCalls++;
			}
			break;

			default:

#endif // WITH_EDITOR

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				if (AllowDebugViewmodes() && bInCollisionView)
				{
					if (bDrawSimpleCollision || bDrawComplexCollision)
					{
						// Override the mesh's material with our material that draws the collision color
						auto CollisionMaterialInstance = new FColoredMaterialRenderProxy(
							GEngine->ShadedLevelColorationUnlitMaterial->GetRenderProxy(),
							GetWireframeColor()
						);
						Collector.RegisterOneFrameMaterialProxy(CollisionMaterialInstance);

						Mesh.MaterialRenderProxy = CollisionMaterialInstance;
						Mesh.bCanApplyViewModeOverrides = true;
						Mesh.bUseWireframeSelectionColoring = IsSelected();

						Collector.AddMesh(ViewIndex, Mesh);

						NumPasses++;
						NumTriangles += Mesh.GetNumPrimitives();
						NumDrawCalls += Mesh.Elements.Num();
					}
				}
				else
#endif
					// Regular CyLand rendering. Only use the dynamic path if we're rendering a rich view or we've disabled the static path for debugging.
					if (IsRichView(ViewFamily) ||
						GCyLandDebugOptions.bDisableStatic ||
						bIsWireframe ||
#if WITH_EDITOR
						(IsSelected() && !GCyLandEditModeActive) ||
#else
						IsSelected() ||
#endif
						!IsStaticPathAvailable())
					{
						Mesh.bCanApplyViewModeOverrides = true;
						Mesh.bUseWireframeSelectionColoring = IsSelected();

						Collector.AddMesh(ViewIndex, Mesh);

						NumPasses++;
						NumTriangles += Mesh.GetNumPrimitives();
						NumDrawCalls += Mesh.Elements.Num();
					}

#if WITH_EDITOR
			} // switch
#endif

#if WITH_EDITOR
			  // Extra render passes for landscape tools
			if (GCyLandEditModeActive)
			{
				// Region selection
				if (EditToolRenderData.SelectedType)
				{
					if ((GCyLandEditRenderMode & ECyLandEditRenderMode::SelectRegion) && (EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_REGION)
						&& !(GCyLandEditRenderMode & ECyLandEditRenderMode::Mask))
					{
						FMeshBatch& SelectMesh = Collector.AllocateMesh();
						SelectMesh = MeshTools;
						auto SelectMaterialInstance = new FCyLandSelectMaterialRenderProxy(GSelectionRegionMaterial->GetRenderProxy(), EditToolRenderData.DataTexture ? EditToolRenderData.DataTexture : GCyLandBlackTexture);
						SelectMesh.MaterialRenderProxy = SelectMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(SelectMaterialInstance);
						Collector.AddMesh(ViewIndex, SelectMesh);
						NumPasses++;
						NumTriangles += SelectMesh.GetNumPrimitives();
						NumDrawCalls += SelectMesh.Elements.Num();
					}

					if ((GCyLandEditRenderMode & ECyLandEditRenderMode::SelectComponent) && (EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_COMPONENT))
					{
						FMeshBatch& SelectMesh = Collector.AllocateMesh();
						SelectMesh = MeshTools;
						SelectMesh.MaterialRenderProxy = GSelectionColorMaterial->GetRenderProxy();
						Collector.AddMesh(ViewIndex, SelectMesh);
						NumPasses++;
						NumTriangles += SelectMesh.GetNumPrimitives();
						NumDrawCalls += SelectMesh.Elements.Num();
					}
				}

				// Mask
				if ((GCyLandEditRenderMode & ECyLandEditRenderMode::SelectRegion) && (GCyLandEditRenderMode & ECyLandEditRenderMode::Mask))
				{
					if (EditToolRenderData.SelectedType & FCyLandEditToolRenderData::ST_REGION)
					{
						FMeshBatch& MaskMesh = Collector.AllocateMesh();
						MaskMesh = MeshTools;
						auto MaskMaterialInstance = new FCyLandMaskMaterialRenderProxy(GMaskRegionMaterial->GetRenderProxy(), EditToolRenderData.DataTexture ? EditToolRenderData.DataTexture : GCyLandBlackTexture, !!(GCyLandEditRenderMode & ECyLandEditRenderMode::InvertedMask));
						MaskMesh.MaterialRenderProxy = MaskMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(MaskMaterialInstance);
						Collector.AddMesh(ViewIndex, MaskMesh);
						NumPasses++;
						NumTriangles += MaskMesh.GetNumPrimitives();
						NumDrawCalls += MaskMesh.Elements.Num();
					}
					else if (!(GCyLandEditRenderMode & ECyLandEditRenderMode::InvertedMask))
					{
						FMeshBatch& MaskMesh = Collector.AllocateMesh();
						MaskMesh = MeshTools;
						auto MaskMaterialInstance = new FCyLandMaskMaterialRenderProxy(GMaskRegionMaterial->GetRenderProxy(), GCyLandBlackTexture, false);
						MaskMesh.MaterialRenderProxy = MaskMaterialInstance;
						Collector.RegisterOneFrameMaterialProxy(MaskMaterialInstance);
						Collector.AddMesh(ViewIndex, MaskMesh);
						NumPasses++;
						NumTriangles += MaskMesh.GetNumPrimitives();
						NumDrawCalls += MaskMesh.Elements.Num();
					}
				}

				// Edit mode tools
				if (EditToolRenderData.ToolMaterial)
				{
					FMeshBatch& EditMesh = Collector.AllocateMesh();
					EditMesh = MeshTools;
					EditMesh.MaterialRenderProxy = EditToolRenderData.ToolMaterial->GetRenderProxy();
					Collector.AddMesh(ViewIndex, EditMesh);
					NumPasses++;
					NumTriangles += EditMesh.GetNumPrimitives();
					NumDrawCalls += EditMesh.Elements.Num();
				}

				if (EditToolRenderData.GizmoMaterial && GCyLandEditRenderMode & ECyLandEditRenderMode::Gizmo)
				{
					FMeshBatch& EditMesh = Collector.AllocateMesh();
					EditMesh = MeshTools;
					EditMesh.MaterialRenderProxy = EditToolRenderData.GizmoMaterial->GetRenderProxy();
					Collector.AddMesh(ViewIndex, EditMesh);
					NumPasses++;
					NumTriangles += EditMesh.GetNumPrimitives();
					NumDrawCalls += EditMesh.Elements.Num();
				}
			}
#endif // WITH_EDITOR

			if (GCyLandDebugOptions.bShowPatches)
			{
				DrawWireBox(Collector.GetPDI(ViewIndex), GetBounds().GetBox(), FColor(255, 255, 0), SDPG_World);
			}

			if (ViewFamily.EngineShowFlags.Bounds)
			{
				RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
			}
		}
	}

	INC_DWORD_STAT_BY(STAT_CyLandComponentRenderPasses, NumPasses);
	INC_DWORD_STAT_BY(STAT_CyLandDrawCalls, NumDrawCalls);
	INC_DWORD_STAT_BY(STAT_CyLandTriangles, NumTriangles * NumPasses);
}

bool FCyLandComponentSceneProxy::CollectOccluderElements(FOccluderElementsCollector& Collector) const
{
	// TODO: implement
	return false;
}

//
// FCyLandVertexBuffer
//
/**
* Initialize the RHI for this rendering resource
*/
void FCyLandVertexBuffer::InitRHI()
{
	//UE_LOG(LogCyLand, Warning, TEXT("ACyLand InitRHI"));
	// create a static vertex buffer
	FRHIResourceCreateInfo CreateInfo;
	void* BufferData = nullptr;
	VertexBufferRHI = RHICreateAndLockVertexBuffer(NumVertices * sizeof(FCyLandVertex), BUF_Static, CreateInfo, BufferData);
	FCyLandVertex* Vertex = (FCyLandVertex*)BufferData;
	int32 VertexIndex = 0;
	for (int32 SubY = 0; SubY < NumSubsections; SubY++)
	{
		for (int32 SubX = 0; SubX < NumSubsections; SubX++)
		{
			for (int32 y = 0; y < SubsectionSizeVerts; y++)
			{
				for (int32 x = 0; x < SubsectionSizeVerts; x++)
				{
					Vertex->VertexX = x;
					Vertex->VertexY = y;
					Vertex->SubX = SubX;
					Vertex->SubY = SubY;
					Vertex++;
					VertexIndex++;
				}
			}
		}
	}
	check(NumVertices == VertexIndex);
	RHIUnlockVertexBuffer(VertexBufferRHI);
}

//
// FCyLandSharedBuffers
//

template <typename INDEX_TYPE>
void FCyLandSharedBuffers::CreateIndexBuffers(ERHIFeatureLevel::Type InFeatureLevel, bool bRequiresAdjacencyInformation)
{
	//UE_LOG(LogCyLand, Warning, TEXT("ACyLand CreateIndexBuffers"));
	if (InFeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		if (!bVertexScoresComputed)
		{
			bVertexScoresComputed = ComputeVertexScores();
		}
	}

	TMap<uint64, INDEX_TYPE> VertexMap;
	INDEX_TYPE VertexCount = 0;
	int32 SubsectionSizeQuads = SubsectionSizeVerts - 1;

	// Layout index buffer to determine best vertex order
	int32 MaxLOD = NumIndexBuffers - 1;
	for (int32 Mip = MaxLOD; Mip >= 0; Mip--)
	{
		int32 LodSubsectionSizeQuads = (SubsectionSizeVerts >> Mip) - 1;

		TArray<INDEX_TYPE> NewIndices;
		int32 ExpectedNumIndices = FMath::Square(NumSubsections) * FMath::Square(LodSubsectionSizeQuads) * 6;
		NewIndices.Empty(ExpectedNumIndices);

		int32& MaxIndexFull = IndexRanges[Mip].MaxIndexFull;
		int32& MinIndexFull = IndexRanges[Mip].MinIndexFull;
		MaxIndexFull = 0;
		MinIndexFull = MAX_int32;

		if (InFeatureLevel <= ERHIFeatureLevel::ES3_1)
		{
			// ES2 version
			float MipRatio = (float)SubsectionSizeQuads / (float)LodSubsectionSizeQuads; // Morph current MIP to base MIP

			for (int32 SubY = 0; SubY < NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < NumSubsections; SubX++)
				{
					TArray<INDEX_TYPE> SubIndices;
					SubIndices.Empty(FMath::Square(LodSubsectionSizeQuads) * 6);

					int32& MaxIndex = IndexRanges[Mip].MaxIndex[SubX][SubY];
					int32& MinIndex = IndexRanges[Mip].MinIndex[SubX][SubY];
					MaxIndex = 0;
					MinIndex = MAX_int32;

					for (int32 y = 0; y < LodSubsectionSizeQuads; y++)
					{
						for (int32 x = 0; x < LodSubsectionSizeQuads; x++)
						{
							int32 x0 = FMath::RoundToInt((float)x * MipRatio);
							int32 y0 = FMath::RoundToInt((float)y * MipRatio);
							int32 x1 = FMath::RoundToInt((float)(x + 1) * MipRatio);
							int32 y1 = FMath::RoundToInt((float)(y + 1) * MipRatio);

							FCyLandVertexRef V00(x0, y0, SubX, SubY);
							FCyLandVertexRef V10(x1, y0, SubX, SubY);
							FCyLandVertexRef V11(x1, y1, SubX, SubY);
							FCyLandVertexRef V01(x0, y1, SubX, SubY);

							uint64 Key00 = V00.MakeKey();
							uint64 Key10 = V10.MakeKey();
							uint64 Key11 = V11.MakeKey();
							uint64 Key01 = V01.MakeKey();

							INDEX_TYPE i00;
							INDEX_TYPE i10;
							INDEX_TYPE i11;
							INDEX_TYPE i01;

							INDEX_TYPE* KeyPtr = VertexMap.Find(Key00);
							if (KeyPtr == nullptr)
							{
								i00 = VertexCount++;
								VertexMap.Add(Key00, i00);
							}
							else
							{
								i00 = *KeyPtr;
							}

							KeyPtr = VertexMap.Find(Key10);
							if (KeyPtr == nullptr)
							{
								i10 = VertexCount++;
								VertexMap.Add(Key10, i10);
							}
							else
							{
								i10 = *KeyPtr;
							}

							KeyPtr = VertexMap.Find(Key11);
							if (KeyPtr == nullptr)
							{
								i11 = VertexCount++;
								VertexMap.Add(Key11, i11);
							}
							else
							{
								i11 = *KeyPtr;
							}

							KeyPtr = VertexMap.Find(Key01);
							if (KeyPtr == nullptr)
							{
								i01 = VertexCount++;
								VertexMap.Add(Key01, i01);
							}
							else
							{
								i01 = *KeyPtr;
							}

							// Update the min/max index ranges
							MaxIndex = FMath::Max<int32>(MaxIndex, i00);
							MinIndex = FMath::Min<int32>(MinIndex, i00);
							MaxIndex = FMath::Max<int32>(MaxIndex, i10);
							MinIndex = FMath::Min<int32>(MinIndex, i10);
							MaxIndex = FMath::Max<int32>(MaxIndex, i11);
							MinIndex = FMath::Min<int32>(MinIndex, i11);
							MaxIndex = FMath::Max<int32>(MaxIndex, i01);
							MinIndex = FMath::Min<int32>(MinIndex, i01);

							SubIndices.Add(i00);
							SubIndices.Add(i11);
							SubIndices.Add(i10);

							SubIndices.Add(i00);
							SubIndices.Add(i01);
							SubIndices.Add(i11);
						}
					}

					// update min/max for full subsection
					MaxIndexFull = FMath::Max<int32>(MaxIndexFull, MaxIndex);
					MinIndexFull = FMath::Min<int32>(MinIndexFull, MinIndex);

					TArray<INDEX_TYPE> NewSubIndices;
					::OptimizeFaces<INDEX_TYPE>(SubIndices, NewSubIndices, 32);
					NewIndices.Append(NewSubIndices);
				}
			}
		}
		else
		{
			// non-ES2 version
			int32 SubOffset = 0;
			for (int32 SubY = 0; SubY < NumSubsections; SubY++)
			{
				for (int32 SubX = 0; SubX < NumSubsections; SubX++)
				{
					int32& MaxIndex = IndexRanges[Mip].MaxIndex[SubX][SubY];
					int32& MinIndex = IndexRanges[Mip].MinIndex[SubX][SubY];
					MaxIndex = 0;
					MinIndex = MAX_int32;

					for (int32 y = 0; y < LodSubsectionSizeQuads; y++)
					{
						for (int32 x = 0; x < LodSubsectionSizeQuads; x++)
						{
							INDEX_TYPE i00 = (x + 0) + (y + 0) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i10 = (x + 1) + (y + 0) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i11 = (x + 1) + (y + 1) * SubsectionSizeVerts + SubOffset;
							INDEX_TYPE i01 = (x + 0) + (y + 1) * SubsectionSizeVerts + SubOffset;

							NewIndices.Add(i00);
							NewIndices.Add(i11);
							NewIndices.Add(i10);

							NewIndices.Add(i00);
							NewIndices.Add(i01);
							NewIndices.Add(i11);

							// Update the min/max index ranges
							MaxIndex = FMath::Max<int32>(MaxIndex, i00);
							MinIndex = FMath::Min<int32>(MinIndex, i00);
							MaxIndex = FMath::Max<int32>(MaxIndex, i10);
							MinIndex = FMath::Min<int32>(MinIndex, i10);
							MaxIndex = FMath::Max<int32>(MaxIndex, i11);
							MinIndex = FMath::Min<int32>(MinIndex, i11);
							MaxIndex = FMath::Max<int32>(MaxIndex, i01);
							MinIndex = FMath::Min<int32>(MinIndex, i01);
						}
					}

					// update min/max for full subsection
					MaxIndexFull = FMath::Max<int32>(MaxIndexFull, MaxIndex);
					MinIndexFull = FMath::Min<int32>(MinIndexFull, MinIndex);

					SubOffset += FMath::Square(SubsectionSizeVerts);
				}
			}

			check(MinIndexFull <= (uint32)((INDEX_TYPE)(~(INDEX_TYPE)0)));
			check(NewIndices.Num() == ExpectedNumIndices);
		}

		// Create and init new index buffer with index data
		FRawStaticIndexBuffer16or32<INDEX_TYPE>* IndexBuffer = (FRawStaticIndexBuffer16or32<INDEX_TYPE>*)IndexBuffers[Mip];
		if (!IndexBuffer)
		{
			IndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
		}
		IndexBuffer->AssignNewBuffer(NewIndices);

		// Delay init resource to keep CPU data until create AdjacencyIndexbuffers
		if (!bRequiresAdjacencyInformation)
		{
			IndexBuffer->InitResource();
		}

		IndexBuffers[Mip] = IndexBuffer;
	}
}

void FCyLandSharedBuffers::CreateOccluderIndexBuffer(int32 NumOccluderVertices)
{
	if (NumOccluderVertices <= 0 || NumOccluderVertices > MAX_uint16)
	{
		return;
	}

	uint16 NumLineQuads = ((uint16)FMath::Sqrt(NumOccluderVertices) - 1);
	uint16 NumLineVtx = NumLineQuads + 1;
	check(NumLineVtx*NumLineVtx == NumOccluderVertices);

	int32 NumTris = NumLineQuads*NumLineQuads * 2;
	int32 NumIndices = NumTris * 3;
	OccluderIndicesSP = MakeShared<FOccluderIndexArray, ESPMode::ThreadSafe>();
	OccluderIndicesSP->SetNumUninitialized(NumIndices, false);

	uint16* OcclusionIndices = OccluderIndicesSP->GetData();
	const uint16 NumLineVtxPlusOne = NumLineVtx + 1;
	const uint16 QuadIndices[2][3] = { {0, NumLineVtx, NumLineVtxPlusOne}, {0, NumLineVtxPlusOne, 1} };
	uint16 QuadOffset = 0;
	int32 Index = 0;
	for (int32 y = 0; y < NumLineQuads; y++)
	{
		for (int32 x = 0; x < NumLineQuads; x++)
		{
			for (int32 i = 0; i < 2; i++)
			{
				OcclusionIndices[Index++] = QuadIndices[i][0] + QuadOffset;
				OcclusionIndices[Index++] = QuadIndices[i][1] + QuadOffset;
				OcclusionIndices[Index++] = QuadIndices[i][2] + QuadOffset;
			}
			QuadOffset++;
		}
		QuadOffset++;
	}

	INC_DWORD_STAT_BY(STAT_CyLandOccluderMem, OccluderIndicesSP->GetAllocatedSize());
}

#if WITH_EDITOR
template <typename INDEX_TYPE>
void FCyLandSharedBuffers::CreateGrassIndexBuffer()
{
	TArray<INDEX_TYPE> NewIndices;

	int32 ExpectedNumIndices = FMath::Square(NumSubsections) * (FMath::Square(SubsectionSizeVerts) * 4 / 3 - 1); // *4/3 is for mips, -1 because we only go down to 2x2 not 1x1
	NewIndices.Empty(ExpectedNumIndices);

	int32 NumMips = FMath::CeilLogTwo(SubsectionSizeVerts);

	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		// Store offset to the start of this mip in the index buffer
		GrassIndexMipOffsets.Add(NewIndices.Num());

		int32 MipSubsectionSizeVerts = SubsectionSizeVerts >> Mip;
		int32 SubOffset = 0;
		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				for (int32 y = 0; y < MipSubsectionSizeVerts; y++)
				{
					for (int32 x = 0; x < MipSubsectionSizeVerts; x++)
					{
						// intentionally using SubsectionSizeVerts not MipSubsectionSizeVerts, this is a vert buffer index not a mip vert index
						NewIndices.Add(x + y * SubsectionSizeVerts + SubOffset);
					}
				}

				// intentionally using SubsectionSizeVerts not MipSubsectionSizeVerts (as above)
				SubOffset += FMath::Square(SubsectionSizeVerts);
			}
		}
	}

	check(NewIndices.Num() == ExpectedNumIndices);

	// Create and init new index buffer with index data
	FRawStaticIndexBuffer16or32<INDEX_TYPE>* IndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
	IndexBuffer->AssignNewBuffer(NewIndices);
	IndexBuffer->InitResource();
	GrassIndexBuffer = IndexBuffer;
}
#endif

FCyLandSharedBuffers::FCyLandSharedBuffers(const int32 InSharedBuffersKey, const int32 InSubsectionSizeQuads, const int32 InNumSubsections, const ERHIFeatureLevel::Type InFeatureLevel, const bool bRequiresAdjacencyInformation, int32 NumOccluderVertices)
	: SharedBuffersKey(InSharedBuffersKey)
	, NumIndexBuffers(FMath::CeilLogTwo(InSubsectionSizeQuads + 1))
	, SubsectionSizeVerts(InSubsectionSizeQuads + 1)
	, NumSubsections(InNumSubsections)
	, VertexFactory(nullptr)
	, VertexBuffer(nullptr)
	, AdjacencyIndexBuffers(nullptr)
	, bUse32BitIndices(false)
#if WITH_EDITOR
	, GrassIndexBuffer(nullptr)
#endif
{
	NumVertices = FMath::Square(SubsectionSizeVerts) * FMath::Square(NumSubsections);
	if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
	{
		// Vertex Buffer cannot be shared
		VertexBuffer = new FCyLandVertexBuffer(InFeatureLevel, NumVertices, SubsectionSizeVerts, NumSubsections);
	}
	IndexBuffers = new FIndexBuffer*[NumIndexBuffers];
	FMemory::Memzero(IndexBuffers, sizeof(FIndexBuffer*)* NumIndexBuffers);
	IndexRanges = new FCyLandIndexRanges[NumIndexBuffers]();

	// See if we need to use 16 or 32-bit index buffers
	if (NumVertices > 65535)
	{
		bUse32BitIndices = true;
		CreateIndexBuffers<uint32>(InFeatureLevel, bRequiresAdjacencyInformation);
#if WITH_EDITOR
		if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
		{
			CreateGrassIndexBuffer<uint32>();
		}
#endif
	}
	else
	{
		CreateIndexBuffers<uint16>(InFeatureLevel, bRequiresAdjacencyInformation);
#if WITH_EDITOR
		if (InFeatureLevel > ERHIFeatureLevel::ES3_1)
		{
			CreateGrassIndexBuffer<uint16>();
		}
#endif
	}

	CreateOccluderIndexBuffer(NumOccluderVertices);
}

FCyLandSharedBuffers::~FCyLandSharedBuffers()
{
	delete VertexBuffer;

	for (int32 i = 0; i < NumIndexBuffers; i++)
	{
		IndexBuffers[i]->ReleaseResource();
		delete IndexBuffers[i];
	}
	delete[] IndexBuffers;
	delete[] IndexRanges;

#if WITH_EDITOR
	if (GrassIndexBuffer)
	{
		GrassIndexBuffer->ReleaseResource();
		delete GrassIndexBuffer;
	}
#endif

	delete AdjacencyIndexBuffers;
	delete VertexFactory;

	if (OccluderIndicesSP.IsValid())
	{
		DEC_DWORD_STAT_BY(STAT_CyLandOccluderMem, OccluderIndicesSP->GetAllocatedSize());
	}
}

template<typename IndexType>
static void BuildCyLandAdjacencyIndexBuffer(int32 LODSubsectionSizeQuads, int32 NumSubsections, const FRawStaticIndexBuffer16or32<IndexType>* Indices, TArray<IndexType>& OutPnAenIndices)
{
	if (Indices && Indices->Num())
	{
		// CyLand use regular grid, so only expand Index buffer works
		// PN AEN Dominant Corner
		uint32 TriCount = LODSubsectionSizeQuads*LODSubsectionSizeQuads * 2;

		uint32 ExpandedCount = 12 * TriCount * NumSubsections * NumSubsections;

		OutPnAenIndices.Empty(ExpandedCount);
		OutPnAenIndices.AddUninitialized(ExpandedCount);

		for (int32 SubY = 0; SubY < NumSubsections; SubY++)
		{
			for (int32 SubX = 0; SubX < NumSubsections; SubX++)
			{
				uint32 SubsectionTriIndex = (SubX + SubY * NumSubsections) * TriCount;

				for (uint32 TriIdx = SubsectionTriIndex; TriIdx < SubsectionTriIndex + TriCount; ++TriIdx)
				{
					uint32 OutStartIdx = TriIdx * 12;
					uint32 InStartIdx = TriIdx * 3;
					OutPnAenIndices[OutStartIdx + 0] = Indices->Get(InStartIdx + 0);
					OutPnAenIndices[OutStartIdx + 1] = Indices->Get(InStartIdx + 1);
					OutPnAenIndices[OutStartIdx + 2] = Indices->Get(InStartIdx + 2);

					OutPnAenIndices[OutStartIdx + 3] = Indices->Get(InStartIdx + 0);
					OutPnAenIndices[OutStartIdx + 4] = Indices->Get(InStartIdx + 1);
					OutPnAenIndices[OutStartIdx + 5] = Indices->Get(InStartIdx + 1);
					OutPnAenIndices[OutStartIdx + 6] = Indices->Get(InStartIdx + 2);
					OutPnAenIndices[OutStartIdx + 7] = Indices->Get(InStartIdx + 2);
					OutPnAenIndices[OutStartIdx + 8] = Indices->Get(InStartIdx + 0);

					OutPnAenIndices[OutStartIdx + 9] = Indices->Get(InStartIdx + 0);
					OutPnAenIndices[OutStartIdx + 10] = Indices->Get(InStartIdx + 1);
					OutPnAenIndices[OutStartIdx + 11] = Indices->Get(InStartIdx + 2);
				}
			}
		}
	}
	else
	{
		OutPnAenIndices.Empty();
	}
}


FCyLandSharedAdjacencyIndexBuffer::FCyLandSharedAdjacencyIndexBuffer(FCyLandSharedBuffers* Buffers)
{
	check(Buffers && Buffers->IndexBuffers);

	// Currently only support PN-AEN-Dominant Corner, which is the only mode for UE4 for now
	IndexBuffers.Empty(Buffers->NumIndexBuffers);

	bool b32BitIndex = Buffers->NumVertices > 65535;
	for (int32 i = 0; i < Buffers->NumIndexBuffers; ++i)
	{
		if (b32BitIndex)
		{
			TArray<uint32> OutPnAenIndices;
			BuildCyLandAdjacencyIndexBuffer<uint32>((Buffers->SubsectionSizeVerts >> i) - 1, Buffers->NumSubsections, (FRawStaticIndexBuffer16or32<uint32>*)Buffers->IndexBuffers[i], OutPnAenIndices);

			FRawStaticIndexBuffer16or32<uint32>* IndexBuffer = new FRawStaticIndexBuffer16or32<uint32>();
			IndexBuffer->AssignNewBuffer(OutPnAenIndices);
			IndexBuffers.Add(IndexBuffer);
		}
		else
		{
			TArray<uint16> OutPnAenIndices;
			BuildCyLandAdjacencyIndexBuffer<uint16>((Buffers->SubsectionSizeVerts >> i) - 1, Buffers->NumSubsections, (FRawStaticIndexBuffer16or32<uint16>*)Buffers->IndexBuffers[i], OutPnAenIndices);

			FRawStaticIndexBuffer16or32<uint16>* IndexBuffer = new FRawStaticIndexBuffer16or32<uint16>();
			IndexBuffer->AssignNewBuffer(OutPnAenIndices);
			IndexBuffers.Add(IndexBuffer);
		}

		IndexBuffers[i]->InitResource();
	}
}

FCyLandSharedAdjacencyIndexBuffer::~FCyLandSharedAdjacencyIndexBuffer()
{
	for (int32 i = 0; i < IndexBuffers.Num(); ++i)
	{
		IndexBuffers[i]->ReleaseResource();
		delete IndexBuffers[i];
	}
}

//
// FCyLandVertexFactoryVertexShaderParameters
//

/** Shader parameters for use with FCyLandVertexFactory */
class FCyLandVertexFactoryVertexShaderParameters : public FVertexFactoryShaderParameters
{
public:
	/**
	* Bind shader constants by name
	* @param	ParameterMap - mapping of named shader constants to indices
	*/
	virtual void Bind(const FShaderParameterMap& ParameterMap) override
	{
		//UE_LOG(LogCyLand, Warning, TEXT("Bind start"));
		HeightmapTextureParameter.Bind(ParameterMap, TEXT("HeightmapTexture"));
		HeightmapTextureParameterSampler.Bind(ParameterMap, TEXT("HeightmapTextureSampler"));
		LodValuesParameter.Bind(ParameterMap, TEXT("LodValues"));
		LodTessellationParameter.Bind(ParameterMap, TEXT("LodTessellationParams"));
		NeighborSectionLodParameter.Bind(ParameterMap, TEXT("NeighborSectionLod"));
		LodBiasParameter.Bind(ParameterMap, TEXT("LodBias"));
		SectionLodsParameter.Bind(ParameterMap, TEXT("SectionLods"));
		XYOffsetTextureParameter.Bind(ParameterMap, TEXT("XYOffsetmapTexture"));
		XYOffsetTextureParameterSampler.Bind(ParameterMap, TEXT("XYOffsetmapTextureSampler"));
		//UE_LOG(LogCyLand, Warning, TEXT("Bind end"));
	}

	/**
	* Serialize shader params to an archive
	* @param	Ar - archive to serialize to
	*/
	virtual void Serialize(FArchive& Ar) override
	{
		//UE_LOG(LogCyLand, Warning, TEXT("Bind Serialize"));
		Ar << HeightmapTextureParameter;
		Ar << HeightmapTextureParameterSampler;
		Ar << LodValuesParameter;
		Ar << LodTessellationParameter;
		Ar << NeighborSectionLodParameter;
		Ar << LodBiasParameter;
		Ar << SectionLodsParameter;
		Ar << XYOffsetTextureParameter;
		Ar << XYOffsetTextureParameterSampler;
	}

	virtual void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		bool bShaderRequiresPositionOnlyStream,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
		) const override
	{
		SCOPE_CYCLE_COUNTER(STAT_CyLandVFDrawTimeVS);

		const FCyLandBatchElementParams* BatchElementParams = (const FCyLandBatchElementParams*)BatchElement.UserData;
		check(BatchElementParams);

		const FCyLandComponentSceneProxy* SceneProxy = BatchElementParams->SceneProxy;

		ShaderBindings.Add(Shader->GetUniformBufferParameter<FCyLandCyUniformShaderParameters>(), *BatchElementParams->CyLandCyUniformShaderParametersResource);

		if (HeightmapTextureParameter.IsBound())
		{
			//UE_LOG(LogCyLand, Warning, TEXT("HeightmapTextureParameter Bind succ!!!"));
			ShaderBindings.AddTexture(HeightmapTextureParameter, HeightmapTextureParameterSampler, TStaticSamplerState<SF_Point>::GetRHI(), SceneProxy->HeightmapTexture->Resource->TextureRHI);
		}

		if (LodValuesParameter.IsBound())
		{
			//UE_LOG(LogCyLand, VeryVerbose, TEXT("LodValuesParameter Bind succ"));
			ShaderBindings.Add(LodValuesParameter, SceneProxy->GetShaderLODValues(BatchElementParams->CurrentLOD));
		}

		int32 SubSectionIndex = BatchElementParams->SubX + BatchElementParams->SubY * SceneProxy->NumSubsections;

		// If we have no custom data for this primitive we will compute of the fly the proper values, this will happen if the shader is not used for normal landscape rendering(i.e grass rendering)
		FCyLandComponentSceneProxy::FViewCustomDataLOD* LODData = (FCyLandComponentSceneProxy::FViewCustomDataLOD*)InView->GetCustomData(SceneProxy->GetPrimitiveSceneInfo()->GetIndex());

		if (LODData != nullptr)
		{
			SceneProxy->PostInitViewCustomData(*InView, LODData);

			if (LodTessellationParameter.IsBound())
			{
				ShaderBindings.Add(LodTessellationParameter, LODData->LodTessellationParams);
			}
				
			if (LodBiasParameter.IsBound())
			{
				check(LODData->LodBias == SceneProxy->GetShaderLODBias());
				ShaderBindings.Add(LodBiasParameter, LODData->LodBias);
			}

			if (SectionLodsParameter.IsBound())
			{
				if (LODData->UseCombinedMeshBatch)
				{
					ShaderBindings.Add(SectionLodsParameter, LODData->ShaderCurrentLOD);
				}
				else // in non combined, only the one representing us as we'll be called 4 times (once per sub section)
				{
					check(SubSectionIndex >= 0);
					FVector4 ShaderCurrentLOD(ForceInitToZero);
					ShaderCurrentLOD.Component(SubSectionIndex) = LODData->ShaderCurrentLOD.Component(SubSectionIndex);

					ShaderBindings.Add(SectionLodsParameter, ShaderCurrentLOD);
				}
			}

			if (NeighborSectionLodParameter.IsBound())
			{
				FVector4 ShaderCurrentNeighborLOD[FCyLandComponentSceneProxy::NEIGHBOR_COUNT] = { FVector4(ForceInitToZero), FVector4(ForceInitToZero), FVector4(ForceInitToZero), FVector4(ForceInitToZero) };

				if (LODData->UseCombinedMeshBatch)
				{					
					int32 SubSectionCount = SceneProxy->NumSubsections == 1 ? 1 : FCyLandComponentSceneProxy::MAX_SUBSECTION_COUNT;

					for (int32 NeighborSubSectionIndex = 0; NeighborSubSectionIndex < SubSectionCount; ++NeighborSubSectionIndex)
					{
						ShaderCurrentNeighborLOD[NeighborSubSectionIndex] = LODData->SubSections[NeighborSubSectionIndex].ShaderCurrentNeighborLOD;
						check(ShaderCurrentNeighborLOD[NeighborSubSectionIndex].X != -1.0f); // they should all match so only check the 1st one for simplicity
					}

					ShaderBindings.Add(NeighborSectionLodParameter, ShaderCurrentNeighborLOD);
				}
				else // in non combined, only the one representing us as we'll be called 4 times (once per sub section)
				{
					check(SubSectionIndex >= 0);
					ShaderCurrentNeighborLOD[SubSectionIndex] = LODData->SubSections[SubSectionIndex].ShaderCurrentNeighborLOD;
					check(ShaderCurrentNeighborLOD[SubSectionIndex].X != -1.0f); // they should all match so only check the 1st one for simplicity

					ShaderBindings.Add(NeighborSectionLodParameter, ShaderCurrentNeighborLOD);
				}
			}			
		}
		else
		{
			float ComponentScreenSize = SceneProxy->GetComponentScreenSize(InView, SceneProxy->CyLandComponent->Bounds.Origin, SceneProxy->ComponentMaxExtend, SceneProxy->CyLandComponent->Bounds.SphereRadius);
			
			FCyLandComponentSceneProxy::FViewCustomDataLOD CurrentLODData;
			SceneProxy->CalculateBatchElementLOD(*InView, ComponentScreenSize, InView->LODDistanceFactor, CurrentLODData, true);
			check(CurrentLODData.UseCombinedMeshBatch);

			if (LodBiasParameter.IsBound())
			{
				ShaderBindings.Add(LodBiasParameter, SceneProxy->GetShaderLODBias());
			}

			if (LodTessellationParameter.IsBound())
			{
				FVector4 LodTessellationParams(ForceInitToZero);
				SceneProxy->ComputeTessellationFalloffShaderValues(CurrentLODData, InView->ViewMatrices.GetProjectionMatrix(), LodTessellationParams.X, LodTessellationParams.Y);

				ShaderBindings.Add(LodTessellationParameter, LodTessellationParams);
			}

			if (SectionLodsParameter.IsBound())
			{
				ShaderBindings.Add(SectionLodsParameter, CurrentLODData.ShaderCurrentLOD);
			}

			if (NeighborSectionLodParameter.IsBound())
			{
				FVector4 CurrentNeighborLOD[4] = { FVector4(ForceInitToZero), FVector4(ForceInitToZero), FVector4(ForceInitToZero), FVector4(ForceInitToZero) };

				for (int32 SubY = 0; SubY < SceneProxy->NumSubsections; SubY++)
				{
					for (int32 SubX = 0; SubX < SceneProxy->NumSubsections; SubX++)
					{
						int32 NeighborSubSectionIndex = SubX + SubY * SceneProxy->NumSubsections;
						SceneProxy->GetShaderCurrentNeighborLOD(*InView, CurrentLODData.SubSections[NeighborSubSectionIndex].fBatchElementCurrentLOD, SceneProxy->NumSubsections > 1 ? SubX : INDEX_NONE, SceneProxy->NumSubsections > 1 ? SubY : INDEX_NONE, NeighborSubSectionIndex, CurrentNeighborLOD[NeighborSubSectionIndex]);
						check(CurrentNeighborLOD[NeighborSubSectionIndex].X != -1.0f); // they should all match so only check the 1st one for simplicity
					}
				}

				ShaderBindings.Add(NeighborSectionLodParameter, CurrentNeighborLOD);
			}				
		}

		if (XYOffsetTextureParameter.IsBound() && SceneProxy->XYOffsetmapTexture)
		{
			ShaderBindings.AddTexture(XYOffsetTextureParameter, XYOffsetTextureParameterSampler, TStaticSamplerState<SF_Point>::GetRHI(), SceneProxy->XYOffsetmapTexture->Resource->TextureRHI);
		}
	}

	virtual uint32 GetSize() const override
	{
		return sizeof(*this);
	}

protected:
	FShaderParameter LodTessellationParameter;
	FShaderParameter LodValuesParameter;
	FShaderParameter NeighborSectionLodParameter;
	FShaderParameter LodBiasParameter;
	FShaderParameter SectionLodsParameter;
	FShaderResourceParameter HeightmapTextureParameter;
	FShaderResourceParameter HeightmapTextureParameterSampler;
	FShaderResourceParameter XYOffsetTextureParameter;
	FShaderResourceParameter XYOffsetTextureParameterSampler;
	TShaderUniformBufferParameter<FCyLandCyUniformShaderParameters> CyLandShaderParameters;
};

//
// FCyLandVertexFactoryPixelShaderParameters
//
/**
* Bind shader constants by name
* @param	ParameterMap - mapping of named shader constants to indices
*/
void FCyLandVertexFactoryPixelShaderParameters::Bind(const FShaderParameterMap& ParameterMap)
{
	NormalmapTextureParameter.Bind(ParameterMap, TEXT("NormalmapTexture"));
	NormalmapTextureParameterSampler.Bind(ParameterMap, TEXT("NormalmapTextureSampler"));
	LocalToWorldNoScalingParameter.Bind(ParameterMap, TEXT("LocalToWorldNoScaling"));
}

/**
* Serialize shader params to an archive
* @param	Ar - archive to serialize to
*/
void FCyLandVertexFactoryPixelShaderParameters::Serialize(FArchive& Ar)
{
	Ar << NormalmapTextureParameter
		<< NormalmapTextureParameterSampler
		<< LocalToWorldNoScalingParameter;
}

void FCyLandVertexFactoryPixelShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* InView,
	const class FMeshMaterialShader* Shader,
	bool bShaderRequiresPositionOnlyStream,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	class FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams
) const
{
	SCOPE_CYCLE_COUNTER(STAT_CyLandVFDrawTimePS);

	const FCyLandBatchElementParams* BatchElementParams = (const FCyLandBatchElementParams*)BatchElement.UserData;

	if (LocalToWorldNoScalingParameter.IsBound())
	{
		ShaderBindings.Add(LocalToWorldNoScalingParameter, *BatchElementParams->LocalToWorldNoScalingPtr);
	}

	if (NormalmapTextureParameter.IsBound())
	{
		const FTexture* NormalmapTexture = BatchElementParams->SceneProxy->NormalmapTexture->Resource;
		ShaderBindings.AddTexture(
			NormalmapTextureParameter,
			NormalmapTextureParameterSampler,
			NormalmapTexture->SamplerStateRHI,
			NormalmapTexture->TextureRHI);
	}
}
 
//
// FCyLandVertexFactory
//

void FCyLandVertexFactory::InitRHI()
{
	// list of declaration items
	FVertexDeclarationElementList Elements;

	// position decls
	Elements.Add(AccessStreamComponent(Data.PositionComponent, 0));

	// create the actual device decls
	InitDeclaration(Elements);
}

FCyLandVertexFactory::FCyLandVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
	: FVertexFactory(InFeatureLevel)
{
	//UE_LOG(LogCyLand, Warning, TEXT("FCyLandVertexFactory onConstruct"));
}

FVertexFactoryShaderParameters* FCyLandVertexFactory::ConstructShaderParameters(EShaderFrequency ShaderFrequency)
{
	switch (ShaderFrequency)
	{
	case SF_Vertex:
		return new FCyLandVertexFactoryVertexShaderParameters();
		break;
	case SF_Pixel:
		return new FCyLandVertexFactoryPixelShaderParameters();
		break;
	default:
		return nullptr;
	}
}

void FCyLandVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);
}


IMPLEMENT_VERTEX_FACTORY_TYPE(FCyLandVertexFactory, "/Project/Private/LandscapeVertexFactory.ush", true, true, true, false, false);
//IMPLEMENT_VERTEX_FACTORY_TYPE(FCyLandVertexFactory, "/Engine/Private/LandscapeVertexFactory.ush", true, true, true, false, false);

/**
* Copy the data from another vertex factory
* @param Other - factory to copy from
*/
void FCyLandVertexFactory::Copy(const FCyLandVertexFactory& Other)
{
	//SetSceneProxy(Other.Proxy());
	FCyLandVertexFactory* VertexFactory = this;
	const FDataType* DataCopy = &Other.Data;
	ENQUEUE_RENDER_COMMAND(FCyLandVertexFactoryCopyData)(
		[VertexFactory, DataCopy](FRHICommandListImmediate& RHICmdList)
		{
			VertexFactory->Data = *DataCopy;
		});
	BeginUpdateResourceRHI(this);
}

//
// FCyLandXYOffsetVertexFactory
//

void FCyLandXYOffsetVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
{
	FCyLandVertexFactory::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("LANDSCAPE_XYOFFSET"), TEXT("1"));
}

IMPLEMENT_VERTEX_FACTORY_TYPE(FCyLandXYOffsetVertexFactory, "/Project/Private/LandscapeVertexFactory.ush", true, true, true, false, false);
//IMPLEMENT_VERTEX_FACTORY_TYPE(FCyLandXYOffsetVertexFactory, "/Engine/Private/LandscapeVertexFactory.ush", true, true, true, false, false);

/** UCyLandMaterialInstanceConstant */
UCyLandMaterialInstanceConstant::UCyLandMaterialInstanceConstant(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsLayerThumbnail = false;
}

class FCyLandMaterialResource : public FMaterialResource
{
	const bool bIsLayerThumbnail;
	const bool bDisableTessellation;
	const bool bMobile;
	const bool bEditorToolUsage;

public:
	FCyLandMaterialResource(UCyLandMaterialInstanceConstant* Parent)
		: bIsLayerThumbnail(Parent->bIsLayerThumbnail)
		, bDisableTessellation(Parent->bDisableTessellation)
		, bMobile(Parent->bMobile)
		, bEditorToolUsage(Parent->bEditorToolUsage)
	{
	}

	void GetShaderMapId(EShaderPlatform Platform, FMaterialShaderMapId& OutId) const override
	{
		FMaterialResource::GetShaderMapId(Platform, OutId);

		if (bIsLayerThumbnail || bDisableTessellation)
		{
			FSHA1 Hash;
			Hash.Update(OutId.BasePropertyOverridesHash.Hash, ARRAY_COUNT(OutId.BasePropertyOverridesHash.Hash));

			const FString HashString = TEXT("bOverride_TessellationMode");
			Hash.UpdateWithString(*HashString, HashString.Len());

			Hash.Final();
			Hash.GetHash(OutId.BasePropertyOverridesHash.Hash);
		}
	}

	bool IsUsedWithLandscape() const override
	{
		return !bIsLayerThumbnail;
	}

	bool IsUsedWithStaticLighting() const override
	{
		if (bIsLayerThumbnail)
		{
			return false;
		}
		return FMaterialResource::IsUsedWithStaticLighting();
	}

	bool IsUsedWithSkeletalMesh()          const override { return false; }
	bool IsUsedWithParticleSystem()        const override { return false; }
	bool IsUsedWithParticleSprites()       const override { return false; }
	bool IsUsedWithBeamTrails()            const override { return false; }
	bool IsUsedWithMeshParticles()         const override { return false; }
	bool IsUsedWithNiagaraSprites()       const override { return false; }
	bool IsUsedWithNiagaraRibbons()       const override { return false; }
	bool IsUsedWithNiagaraMeshParticles()       const override { return false; }
	bool IsUsedWithMorphTargets()          const override { return false; }
	bool IsUsedWithSplineMeshes()          const override { return false; }
	bool IsUsedWithInstancedStaticMeshes() const override { return false; }
	bool IsUsedWithAPEXCloth()             const override { return false; }
	bool IsUsedWithGeometryCache()         const override { return false; }
	EMaterialTessellationMode GetTessellationMode() const override { return (bIsLayerThumbnail || bDisableTessellation) ? MTM_NoTessellation : FMaterialResource::GetTessellationMode(); };

	bool ShouldCache(EShaderPlatform Platform, const FShaderType* ShaderType, const FVertexFactoryType* VertexFactoryType) const override
	{
		// Don't compile if this is a mobile shadermap and a desktop MIC, and vice versa, unless it's a tool material
		if (!(IsPCPlatform(Platform) && bEditorToolUsage) && bMobile != IsMobilePlatform(Platform))
		{
			// @todo For some reason this causes this resource to return true for IsCompilationFinished. For now we will needlessly compile this shader until this is fixed.
			//return false;
		}

		if (VertexFactoryType)
		{
			// Always check against FLocalVertexFactory in editor builds as it is required to render thumbnails
			// Thumbnail MICs are only rendered in the preview scene using a simple LocalVertexFactory
			if (bIsLayerThumbnail)
			{
				static const FName LocalVertexFactory = FName(TEXT("FLocalVertexFactory"));
				if (!IsMobilePlatform(Platform) && VertexFactoryType->GetFName() == LocalVertexFactory)
				{
					if (Algo::Find(GetAllowedShaderTypes(), ShaderType->GetFName()))
					{
						return FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
					}
					else
					{
						if (Algo::Find(GetExcludedShaderTypes(), ShaderType->GetFName()))
						{
							UE_LOG(LogCyLand, VeryVerbose, TEXT("Excluding shader %s from landscape thumbnail material"), ShaderType->GetName());
							return false;
						}
						else
						{
							if (Platform == EShaderPlatform::SP_PCD3D_SM5)
							{
								UE_LOG(LogCyLand, Warning, TEXT("Shader %s unknown by landscape thumbnail material, please add to either AllowedShaderTypes or ExcludedShaderTypes"), ShaderType->GetName());
							}
							return FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
						}
					}
				}
			}
			else
			{
				// CyLand MICs are only for use with the CyLand vertex factories
				// Todo: only compile CyLandXYOffsetVertexFactory if we are using it
				static const FName CyLandVertexFactory = FName(TEXT("FCyLandVertexFactory"));
				static const FName CyLandXYOffsetVertexFactory = FName(TEXT("FCyLandXYOffsetVertexFactory"));
				static const FName CyLandVertexFactoryMobile = FName(TEXT("FCyLandVertexFactoryMobile"));
				if (VertexFactoryType->GetFName() == CyLandVertexFactory ||
					VertexFactoryType->GetFName() == CyLandXYOffsetVertexFactory ||
					VertexFactoryType->GetFName() == CyLandVertexFactoryMobile)
				{
					return FMaterialResource::ShouldCache(Platform, ShaderType, VertexFactoryType);
				}
			}
		}

		return false;
	}

	static const TArray<FName>& GetAllowedShaderTypes()
	{
		// reduce the number of shaders compiled for the thumbnail materials by only compiling with shader types known to be used by the preview scene
		static const TArray<FName> AllowedShaderTypes =
		{
			FName(TEXT("TBasePassVSFNoLightMapPolicy")),
			FName(TEXT("TBasePassPSFNoLightMapPolicy")),
			FName(TEXT("TBasePassVSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OutputDepthfalse")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OutputDepthtrue")), // used by LPV
			FName(TEXT("TShadowDepthPSPixelShadowDepth_NonPerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_NonPerspectiveCorrecttrue")), // used by LPV
			FName(TEXT("TBasePassPSFSimpleDirectionalLightLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleDirectionalLightLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleDirectionalLightLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleNoLightmapLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleNoLightmapLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleNoLightmapLightingPolicy")),
			FName(TEXT("TBasePassVSFSimpleNoLightmapLightingPolicyAtmosphericFog")),
			FName(TEXT("TDepthOnlyVS<false>")),
			FName(TEXT("TDepthOnlyVS<true>")),
			FName(TEXT("FDepthOnlyPS")),
			// UE-44519, masked material with landscape layers requires FHitProxy shaders.
			FName(TEXT("FHitProxyVS")),
			FName(TEXT("FHitProxyPS")),
			FName(TEXT("FVelocityVS")),
			FName(TEXT("FVelocityPS")),

			FName(TEXT("TBasePassPSFSimpleStationaryLightSingleSampleShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightSingleSampleShadowsLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightSingleSampleShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightPrecomputedShadowsLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleStationaryLightPrecomputedShadowsLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightPrecomputedShadowsLightingPolicy")),
			FName(TEXT("TBasePassVSFNoLightMapPolicyAtmosphericFog")),
			FName(TEXT("TBasePassDSFNoLightMapPolicy")),
			FName(TEXT("TBasePassHSFNoLightMapPolicy")),
			FName(TEXT("TLightMapDensityVSFNoLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSFNoLightMapPolicy")),

			// Mobile
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMLightingPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMLightingPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMLightingPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMLightingPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightCSMLightingPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightLightingPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightLightingPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightLightingPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightLightingPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightLightingPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDirectionalLightCSMAndSHIndirectPolicyHDRLinear64")),

			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightCSMAndSHIndirectPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),

			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightAndSHIndirectPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDirectionalLightAndSHIndirectPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFNoLightMapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFNoLightMapPolicyHDRLinear64")),

			// Forward shading required
			FName(TEXT("TBasePassPSFCachedPointIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassPSFNoLightMapPolicySkylight")),
		};
		return AllowedShaderTypes;
	}

	static const TArray<FName>& GetExcludedShaderTypes()
	{
		// shader types known *not* to be used by the preview scene
		static const TArray<FName> ExcludedShaderTypes =
		{
			// This is not an exhaustive list
			FName(TEXT("FDebugViewModeVS")),
			FName(TEXT("FConvertToCyUniformMeshVS")),
			FName(TEXT("FConvertToCyUniformMeshGS")),

			// No lightmap on thumbnails
			FName(TEXT("TLightMapDensityVSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityPSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityVSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityPSTLightMapPolicyLQ")),
			FName(TEXT("TLightMapDensityVSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassPSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTDistanceFieldShadowsAndLightMapPolicyHQSkylight")),
			FName(TEXT("TBasePassVSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyHQSkylight")),
			FName(TEXT("TBasePassVSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassPSTLightMapPolicyLQSkylight")),
			FName(TEXT("TBasePassVSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassVSFSimpleStationaryLightVolumetricLightmapShadowsLightingPolicy")),

			// Mobile
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightCSMWithLightmapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightCSMWithLightmapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileMovableDirectionalLightWithLightmapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileMovableDirectionalLightWithLightmapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDistanceFieldShadowsLightMapAndCSMLightingPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicyINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicyINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicy0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSFMobileDistanceFieldShadowsAndLQLightMapPolicy0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSFMobileDistanceFieldShadowsAndLQLightMapPolicyHDRLinear64")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQINT32_MAXHDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQINT32_MAXHDRLinear64")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQ0HDRLinear64Skylight")),
			FName(TEXT("TMobileBasePassPSTLightMapPolicyLQ0HDRLinear64")),
			FName(TEXT("TMobileBasePassVSTLightMapPolicyLQHDRLinear64")),

			FName(TEXT("TBasePassVSFCachedVolumeIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedVolumeIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFCachedVolumeIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassPSFPrecomputedVolumetricLightmapLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFPrecomputedVolumetricLightmapLightingPolicy")),
			FName(TEXT("TBasePassPSFPrecomputedVolumetricLightmapLightingPolicy")),

			FName(TEXT("TBasePassPSFSimpleStationaryLightVolumetricLightmapShadowsLightingPolicy")),

			FName(TEXT("TBasePassVSFCachedPointIndirectLightingPolicyAtmosphericFog")),
			FName(TEXT("TBasePassVSFSelfShadowedCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedCachedPointIndirectLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSelfShadowedCachedPointIndirectLightingPolicyAtmosphericFog")),
			FName(TEXT("TBasePassVSFSelfShadowedTranslucencyPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedTranslucencyPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedTranslucencyPolicySkylight")),
			FName(TEXT("TBasePassVSFSelfShadowedTranslucencyPolicyAtmosphericFog")),

			FName(TEXT("TShadowDepthVSVertexShadowDepth_PerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_PerspectiveCorrecttrue")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OnePassPointLightfalse")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_PerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_PerspectiveCorrecttrue")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_OnePassPointLightfalse")),
			FName(TEXT("TShadowDepthPSPixelShadowDepth_OnePassPointLighttrue")),

			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OutputDepthfalse")),
			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OutputDepthtrue")),
			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_PerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_PerspectiveCorrecttrue")),
			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OnePassPointLightfalse")),
			FName(TEXT("FOnePassPointShadowDepthGS")),

			FName(TEXT("TTranslucencyShadowDepthVS<TranslucencyShadowDepth_Standard>")),
			FName(TEXT("TTranslucencyShadowDepthPS<TranslucencyShadowDepth_Standard>")),
			FName(TEXT("TTranslucencyShadowDepthVS<TranslucencyShadowDepth_PerspectiveCorrect>")),
			FName(TEXT("TTranslucencyShadowDepthPS<TranslucencyShadowDepth_PerspectiveCorrect>")),

			FName(TEXT("TShadowDepthVSForGSVertexShadowDepth_OnePassPointLightPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly")),
			FName(TEXT("TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly")),

			FName(TEXT("TBasePassVSTDistanceFieldShadowsAndLightMapPolicyHQAtmosphericFog")),
			FName(TEXT("TBasePassVSTLightMapPolicyHQAtmosphericFog")),
			FName(TEXT("TBasePassVSTLightMapPolicyLQAtmosphericFog")),
			FName(TEXT("TBasePassVSFPrecomputedVolumetricLightmapLightingPolicyAtmosphericFog")),
			FName(TEXT("TBasePassPSFSelfShadowedVolumetricLightmapPolicy")),
			FName(TEXT("TBasePassPSFSelfShadowedVolumetricLightmapPolicySkylight")),
			FName(TEXT("TBasePassVSFSelfShadowedVolumetricLightmapPolicyAtmosphericFog")),
			FName(TEXT("TBasePassVSFSelfShadowedVolumetricLightmapPolicy")),

			FName(TEXT("TBasePassPSFSimpleLightmapOnlyLightingPolicy")),
			FName(TEXT("TBasePassPSFSimpleLightmapOnlyLightingPolicySkylight")),
			FName(TEXT("TBasePassVSFSimpleLightmapOnlyLightingPolicy")),

			FName(TEXT("TShadowDepthDSVertexShadowDepth_OnePassPointLightfalse")),
			FName(TEXT("TShadowDepthHSVertexShadowDepth_OnePassPointLightfalse")),
			FName(TEXT("TShadowDepthDSVertexShadowDepth_OutputDepthfalse")),
			FName(TEXT("TShadowDepthHSVertexShadowDepth_OutputDepthfalse")),
			FName(TEXT("TShadowDepthDSVertexShadowDepth_OutputDepthtrue")),
			FName(TEXT("TShadowDepthHSVertexShadowDepth_OutputDepthtrue")),

			FName(TEXT("TShadowDepthDSVertexShadowDepth_PerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthHSVertexShadowDepth_PerspectiveCorrectfalse")),
			FName(TEXT("TShadowDepthDSVertexShadowDepth_PerspectiveCorrecttrue")),
			FName(TEXT("TShadowDepthHSVertexShadowDepth_PerspectiveCorrecttrue")),

			FName(TEXT("FVelocityDS")),
			FName(TEXT("FVelocityHS")),
			FName(TEXT("FHitProxyDS")),
			FName(TEXT("FHitProxyHS")),

			FName(TEXT("TLightMapDensityDSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityHSTLightMapPolicyHQ")),
			FName(TEXT("TLightMapDensityDSTLightMapPolicyLQ")),
			FName(TEXT("TLightMapDensityHSTLightMapPolicyLQ")),
			FName(TEXT("TLightMapDensityDSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityHSFDummyLightMapPolicy")),
			FName(TEXT("TLightMapDensityDSFNoLightMapPolicy")),
			FName(TEXT("TLightMapDensityHSFNoLightMapPolicy")),
			FName(TEXT("FDepthOnlyDS")),
			FName(TEXT("FDepthOnlyHS")),
			FName(TEXT("FDebugViewModeDS")),
			FName(TEXT("FDebugViewModeHS")),
			FName(TEXT("TBasePassDSTDistanceFieldShadowsAndLightMapPolicyHQ")),
			FName(TEXT("TBasePassHSTDistanceFieldShadowsAndLightMapPolicyHQ")),

			FName(TEXT("TBasePassDSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassHSTLightMapPolicyHQ")),
			FName(TEXT("TBasePassDSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassHSTLightMapPolicyLQ")),
			FName(TEXT("TBasePassDSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassHSFCachedPointIndirectLightingPolicy")),
			FName(TEXT("TBasePassDSFCachedVolumeIndirectLightingPolicy")),
			FName(TEXT("TBasePassHSFCachedVolumeIndirectLightingPolicy")),

			FName(TEXT("TBasePassDSFPrecomputedVolumetricLightmapLightingPolicy")),
			FName(TEXT("TBasePassHSFPrecomputedVolumetricLightmapLightingPolicy")),
		};
		return ExcludedShaderTypes;
	}
};

FMaterialResource* UCyLandMaterialInstanceConstant::AllocatePermutationResource()
{
	return new FCyLandMaterialResource(this);
}

bool UCyLandMaterialInstanceConstant::HasOverridenBaseProperties() const
{
	if (Parent)
	{
		// force a static permutation for UCyLandMaterialInstanceConstants
		if (!Parent->IsA<UCyLandMaterialInstanceConstant>())
		{
			return true;
		}
		UCyLandMaterialInstanceConstant* CyLandMICParent = CastChecked<UCyLandMaterialInstanceConstant>(Parent);
		if (bDisableTessellation != CyLandMICParent->bDisableTessellation)
		{
			return true;
		}
	}

	return Super::HasOverridenBaseProperties();
}

//////////////////////////////////////////////////////////////////////////

void UCyLandComponent::GetStreamingTextureInfo(FStreamingTextureLevelContext& LevelContext, TArray<FStreamingTexturePrimitiveInfo>& OutStreamingTextures) const
{
	ACyLandProxy* Proxy = Cast<ACyLandProxy>(GetOuter());
	FSphere BoundingSphere = Bounds.GetSphere();
	float LocalStreamingDistanceMultiplier = 1.f;
	float TexelFactor = 0.0f;
	if (Proxy)
	{
		LocalStreamingDistanceMultiplier = FMath::Max(0.0f, Proxy->StreamingDistanceMultiplier);
		TexelFactor = 0.75f * LocalStreamingDistanceMultiplier * ComponentSizeQuads * FMath::Abs(Proxy->GetRootComponent()->RelativeScale3D.X);
	}

	ERHIFeatureLevel::Type FeatureLevel = LevelContext.GetFeatureLevel();
	int32 MaterialInstanceCount = FeatureLevel >= ERHIFeatureLevel::SM4 ? GetMaterialInstanceCount() : MobileMaterialInterfaces.Num();

	for (int32 MaterialIndex = 0; MaterialIndex < MaterialInstanceCount; ++MaterialIndex)
	{
		const UMaterialInterface* MaterialInterface = FeatureLevel >= ERHIFeatureLevel::SM4 ? GetMaterialInstance(MaterialIndex) : MobileMaterialInterfaces[MaterialIndex];

		// Normal usage...
		// Enumerate the textures used by the material.
		if (MaterialInterface)
		{
			TArray<UTexture*> Textures;
			MaterialInterface->GetUsedTextures(Textures, EMaterialQualityLevel::Num, false, FeatureLevel, false);
			// Add each texture to the output with the appropriate parameters.
			// TODO: Take into account which UVIndex is being used.
			for (int32 TextureIndex = 0; TextureIndex < Textures.Num(); TextureIndex++)
			{
				UTexture2D* Texture2D = Cast<UTexture2D>(Textures[TextureIndex]);
				if (!Texture2D) continue;

				FStreamingTexturePrimitiveInfo& StreamingTexture = *new(OutStreamingTextures)FStreamingTexturePrimitiveInfo;
				StreamingTexture.Bounds = BoundingSphere;
				StreamingTexture.TexelFactor = TexelFactor;
				StreamingTexture.Texture = Texture2D;
			}

			const UMaterial* Material = MaterialInterface->GetMaterial();
			if (Material)
			{
				int32 NumExpressions = Material->Expressions.Num();
				for (int32 ExpressionIndex = 0; ExpressionIndex < NumExpressions; ExpressionIndex++)
				{
					UMaterialExpression* Expression = Material->Expressions[ExpressionIndex];
					UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression);

					// TODO: This is only works for direct Coordinate Texture Sample cases
					if (TextureSample && TextureSample->Coordinates.IsConnected())
					{
						UMaterialExpressionTextureCoordinate* TextureCoordinate = nullptr;
						UMaterialExpressionCyLandLayerCoords* TerrainTextureCoordinate = nullptr;

						for (UMaterialExpression* FindExp : Material->Expressions)
						{
							if (FindExp && FindExp->GetFName() == TextureSample->Coordinates.ExpressionName)
							{
								TextureCoordinate = Cast<UMaterialExpressionTextureCoordinate>(FindExp);
								if (!TextureCoordinate)
								{
									TerrainTextureCoordinate = Cast<UMaterialExpressionCyLandLayerCoords>(FindExp);
								}
								break;
							}
						}

						if (TextureCoordinate || TerrainTextureCoordinate)
						{
							for (int32 i = 0; i < OutStreamingTextures.Num(); ++i)
							{
								FStreamingTexturePrimitiveInfo& StreamingTexture = OutStreamingTextures[i];
								if (StreamingTexture.Texture == TextureSample->Texture)
								{
									if (TextureCoordinate)
									{
										StreamingTexture.TexelFactor = TexelFactor * FPlatformMath::Max(TextureCoordinate->UTiling, TextureCoordinate->VTiling);
									}
									else //if ( TerrainTextureCoordinate )
									{
										StreamingTexture.TexelFactor = TexelFactor * TerrainTextureCoordinate->MappingScale;
									}
									break;
								}
							}
						}
					}
				}
			}

			// Lightmap
			const FMeshMapBuildData* MapBuildData = GetMeshMapBuildData();

			FLightMap2D* Lightmap = MapBuildData && MapBuildData->LightMap ? MapBuildData->LightMap->GetLightMap2D() : nullptr;
			uint32 LightmapIndex = AllowHighQualityLightmaps(FeatureLevel) ? 0 : 1;
			if (Lightmap && Lightmap->IsValid(LightmapIndex))
			{
				const FVector2D& Scale = Lightmap->GetCoordinateScale();
				if (Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER)
				{
					const float LightmapTexelFactor = TexelFactor / FMath::Min(Scale.X, Scale.Y);
					new (OutStreamingTextures) FStreamingTexturePrimitiveInfo(Lightmap->GetTexture(LightmapIndex), Bounds, LightmapTexelFactor);
					new (OutStreamingTextures) FStreamingTexturePrimitiveInfo(Lightmap->GetAOMaterialMaskTexture(), Bounds, LightmapTexelFactor);
					new (OutStreamingTextures) FStreamingTexturePrimitiveInfo(Lightmap->GetSkyOcclusionTexture(), Bounds, LightmapTexelFactor);
				}
			}

			// Shadowmap
			FShadowMap2D* Shadowmap = MapBuildData && MapBuildData->ShadowMap ? MapBuildData->ShadowMap->GetShadowMap2D() : nullptr;
			if (Shadowmap && Shadowmap->IsValid())
			{
				const FVector2D& Scale = Shadowmap->GetCoordinateScale();
				if (Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER)
				{
					const float ShadowmapTexelFactor = TexelFactor / FMath::Min(Scale.X, Scale.Y);
					new (OutStreamingTextures) FStreamingTexturePrimitiveInfo(Shadowmap->GetTexture(), Bounds, ShadowmapTexelFactor);
				}
			}
		}
	}

	// Weightmap
	for (int32 TextureIndex = 0; TextureIndex < WeightmapTextures.Num(); TextureIndex++)
	{
		FStreamingTexturePrimitiveInfo& StreamingWeightmap = *new(OutStreamingTextures)FStreamingTexturePrimitiveInfo;
		StreamingWeightmap.Bounds = BoundingSphere;
		StreamingWeightmap.TexelFactor = TexelFactor;
		StreamingWeightmap.Texture = WeightmapTextures[TextureIndex];
	}

	// Heightmap
	if (HeightmapTexture)
	{
		FStreamingTexturePrimitiveInfo& StreamingHeightmap = *new(OutStreamingTextures)FStreamingTexturePrimitiveInfo;
		StreamingHeightmap.Bounds = BoundingSphere;

		float HeightmapTexelFactor = TexelFactor * (static_cast<float>(HeightmapTexture->GetSizeY()) / (ComponentSizeQuads + 1));
		StreamingHeightmap.TexelFactor = ForcedLOD >= 0 ? -(1 << (13 - ForcedLOD)) : HeightmapTexelFactor; // Minus Value indicate forced resolution (Mip 13 for 8k texture)
		StreamingHeightmap.Texture = HeightmapTexture;
	}

	// XYOffset
	if (XYOffsetmapTexture)
	{
		FStreamingTexturePrimitiveInfo& StreamingXYOffset = *new(OutStreamingTextures)FStreamingTexturePrimitiveInfo;
		StreamingXYOffset.Bounds = BoundingSphere;
		StreamingXYOffset.TexelFactor = TexelFactor;
		StreamingXYOffset.Texture = XYOffsetmapTexture;
	}

#if WITH_EDITOR
	if (GIsEditor && EditToolRenderData.DataTexture)
	{
		FStreamingTexturePrimitiveInfo& StreamingDatamap = *new(OutStreamingTextures)FStreamingTexturePrimitiveInfo;
		StreamingDatamap.Bounds = BoundingSphere;
		StreamingDatamap.TexelFactor = TexelFactor;
		StreamingDatamap.Texture = EditToolRenderData.DataTexture;
	}
#endif
}

void ACyLandProxy::ChangeTessellationComponentScreenSize(float InTessellationComponentScreenSize)
{
	TessellationComponentScreenSize = FMath::Clamp<float>(InTessellationComponentScreenSize, 0.01f, 1.0f);

	if (CyLandComponents.Num() > 0)
	{
		int32 ComponentCount = CyLandComponents.Num();
		FCyLandComponentSceneProxy** RenderProxies = new FCyLandComponentSceneProxy*[ComponentCount];
		for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
		{
			RenderProxies[Idx] = (FCyLandComponentSceneProxy*)(CyLandComponents[Idx]->SceneProxy);
		}

		float TessellationComponentScreenSizeLocal = TessellationComponentScreenSize;
		ENQUEUE_RENDER_COMMAND(CyLandChangeTessellationComponentScreenSizeCommand)(
			[RenderProxies, ComponentCount, TessellationComponentScreenSizeLocal](FRHICommandListImmediate& RHICmdList)
			{
				for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
				{
					if (RenderProxies[Idx] != nullptr)
					{
						RenderProxies[Idx]->ChangeTessellationComponentScreenSize_RenderThread(TessellationComponentScreenSizeLocal);
					}
				}

				delete[] RenderProxies;
			}
		);
	}
}

void ACyLandProxy::ChangeComponentScreenSizeToUseSubSections(float InComponentScreenSizeToUseSubSections)
{
	ComponentScreenSizeToUseSubSections = FMath::Clamp<float>(InComponentScreenSizeToUseSubSections, 0.01f, 1.0f);

	if (CyLandComponents.Num() > 0)
	{
		int32 ComponentCount = CyLandComponents.Num();
		FCyLandComponentSceneProxy** RenderProxies = new FCyLandComponentSceneProxy*[ComponentCount];
		for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
		{
			RenderProxies[Idx] = (FCyLandComponentSceneProxy*)(CyLandComponents[Idx]->SceneProxy);
		}

		float ComponentScreenSizeToUseSubSectionsLocal = ComponentScreenSizeToUseSubSections;
		ENQUEUE_RENDER_COMMAND(CyLandChangeComponentScreenSizeToUseSubSectionsCommand)(
			[RenderProxies, ComponentCount, ComponentScreenSizeToUseSubSectionsLocal](FRHICommandListImmediate& RHICmdList)
			{
				for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
				{
					if (RenderProxies[Idx] != nullptr)
					{
						RenderProxies[Idx]->ChangeComponentScreenSizeToUseSubSections_RenderThread(ComponentScreenSizeToUseSubSectionsLocal);
					}
				}

				delete[] RenderProxies;
			}
		);
	}
}

void ACyLandProxy::ChangeUseTessellationComponentScreenSizeFalloff(bool InUseTessellationComponentScreenSizeFalloff)
{
	UseTessellationComponentScreenSizeFalloff = InUseTessellationComponentScreenSizeFalloff;

	if (CyLandComponents.Num() > 0)
	{
		int32 ComponentCount = CyLandComponents.Num();
		FCyLandComponentSceneProxy** RenderProxies = new FCyLandComponentSceneProxy*[ComponentCount];
		for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
		{
			RenderProxies[Idx] = (FCyLandComponentSceneProxy*)(CyLandComponents[Idx]->SceneProxy);
		}

		ENQUEUE_RENDER_COMMAND(CyLandChangeUseTessellationComponentScreenSizeFalloffCommand)(
			[RenderProxies, ComponentCount, InUseTessellationComponentScreenSizeFalloff](FRHICommandListImmediate& RHICmdList)
			{
				for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
				{
					if (RenderProxies[Idx] != nullptr)
					{
						RenderProxies[Idx]->ChangeUseTessellationComponentScreenSizeFalloff_RenderThread(InUseTessellationComponentScreenSizeFalloff);
					}
				}

				delete[] RenderProxies;
			}
		);
	}
}

void ACyLandProxy::ChangeTessellationComponentScreenSizeFalloff(float InTessellationComponentScreenSizeFalloff)
{
	TessellationComponentScreenSizeFalloff = FMath::Clamp<float>(TessellationComponentScreenSizeFalloff, 0.01f, 1.0f);

	if (CyLandComponents.Num() > 0)
	{
		int32 ComponentCount = CyLandComponents.Num();
		FCyLandComponentSceneProxy** RenderProxies = new FCyLandComponentSceneProxy*[ComponentCount];
		for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
		{
			RenderProxies[Idx] = (FCyLandComponentSceneProxy*)(CyLandComponents[Idx]->SceneProxy);
		}

		float TessellationComponentScreenSizeFalloffLocal = TessellationComponentScreenSizeFalloff;
		ENQUEUE_RENDER_COMMAND(CyLandChangeTessellationComponentScreenSizeFalloffCommand)(
			[RenderProxies, ComponentCount, TessellationComponentScreenSizeFalloffLocal](FRHICommandListImmediate& RHICmdList)
			{
				for (int32 Idx = 0; Idx < ComponentCount; ++Idx)
				{
					if (RenderProxies[Idx] != nullptr)
					{
						RenderProxies[Idx]->ChangeTessellationComponentScreenSizeFalloff_RenderThread(TessellationComponentScreenSizeFalloffLocal);
					}
				}

				delete[] RenderProxies;
			}
		);
	}
}

void ACyLandProxy::ChangeLODDistanceFactor(float InLODDistanceFactor)
{
	// Deprecated
}

void FCyLandComponentSceneProxy::ChangeTessellationComponentScreenSize_RenderThread(float InTessellationComponentScreenSize)
{
	TessellationComponentSquaredScreenSize = FMath::Square(InTessellationComponentScreenSize);
}

void FCyLandComponentSceneProxy::ChangeComponentScreenSizeToUseSubSections_RenderThread(float InComponentScreenSizeToUseSubSections)
{
	ComponentSquaredScreenSizeToUseSubSections = FMath::Square(InComponentScreenSizeToUseSubSections);
}

void FCyLandComponentSceneProxy::ChangeUseTessellationComponentScreenSizeFalloff_RenderThread(bool InUseTessellationComponentScreenSizeFalloff)
{
	UseTessellationComponentScreenSizeFalloff = InUseTessellationComponentScreenSizeFalloff;
}

void FCyLandComponentSceneProxy::ChangeTessellationComponentScreenSizeFalloff_RenderThread(float InTessellationComponentScreenSizeFalloff)
{
	TessellationComponentScreenSizeFalloff = InTessellationComponentScreenSizeFalloff;
}

bool FCyLandComponentSceneProxy::HeightfieldHasPendingStreaming() const
{
	return HeightmapTexture && HeightmapTexture->bHasStreamingUpdatePending;
}

void FCyLandComponentSceneProxy::GetHeightfieldRepresentation(UTexture2D*& OutHeightmapTexture, UTexture2D*& OutDiffuseColorTexture, FHeightfieldComponentDescription& OutDescription)
{
	OutHeightmapTexture = HeightmapTexture;
	OutDiffuseColorTexture = BaseColorForGITexture;
	OutDescription.HeightfieldScaleBias = HeightmapScaleBias;

	OutDescription.MinMaxUV = FVector4(
		HeightmapScaleBias.Z,
		HeightmapScaleBias.W,
		HeightmapScaleBias.Z + SubsectionSizeVerts * NumSubsections * HeightmapScaleBias.X - HeightmapScaleBias.X,
		HeightmapScaleBias.W + SubsectionSizeVerts * NumSubsections * HeightmapScaleBias.Y - HeightmapScaleBias.Y);

	OutDescription.HeightfieldRect = FIntRect(SectionBase.X, SectionBase.Y, SectionBase.X + NumSubsections * SubsectionSizeQuads, SectionBase.Y + NumSubsections * SubsectionSizeQuads);

	OutDescription.NumSubsections = NumSubsections;

	OutDescription.SubsectionScaleAndBias = FVector4(SubsectionSizeQuads, SubsectionSizeQuads, HeightmapSubsectionOffsetU, HeightmapSubsectionOffsetV);
}

void FCyLandComponentSceneProxy::GetLCIs(FLCIArray& LCIs)
{
	FLightCacheInterface* LCI = ComponentLightInfo.Get();
	if (LCI)
	{
		LCIs.Push(LCI);
	}
}

//
// FCyLandNeighborInfo
//
void FCyLandNeighborInfo::RegisterNeighbors()
{
	if (!bRegistered)
	{
		// Register ourselves in the map.
		TMap<FIntPoint, const FCyLandNeighborInfo*>& SceneProxyMap = SharedSceneProxyMap.FindOrAdd(CyLandKey);

		const FCyLandNeighborInfo* Existing = SceneProxyMap.FindRef(ComponentBase);
		if (Existing == nullptr)//(ensure(Existing == nullptr))
		{
			SceneProxyMap.Add(ComponentBase, this);
			bRegistered = true;

			// Find Neighbors
			Neighbors[0] = SceneProxyMap.FindRef(ComponentBase + FIntPoint(0, -1));
			Neighbors[1] = SceneProxyMap.FindRef(ComponentBase + FIntPoint(-1, 0));
			Neighbors[2] = SceneProxyMap.FindRef(ComponentBase + FIntPoint(1, 0));
			Neighbors[3] = SceneProxyMap.FindRef(ComponentBase + FIntPoint(0, 1));

			// Add ourselves to our neighbors
			if (Neighbors[0])
			{
				Neighbors[0]->Neighbors[3] = this;
			}
			if (Neighbors[1])
			{
				Neighbors[1]->Neighbors[2] = this;
			}
			if (Neighbors[2])
			{
				Neighbors[2]->Neighbors[1] = this;
			}
			if (Neighbors[3])
			{
				Neighbors[3]->Neighbors[0] = this;
			}
		}
		else
		{
			UE_LOG(LogCyLand, Warning, TEXT("Duplicate ComponentBase %d, %d"), ComponentBase.X, ComponentBase.Y);
		}
	}
}

void FCyLandNeighborInfo::UnregisterNeighbors()
{
	if (bRegistered)
	{
		// Remove ourselves from the map
		TMap<FIntPoint, const FCyLandNeighborInfo*>* SceneProxyMap = SharedSceneProxyMap.Find(CyLandKey);
		check(SceneProxyMap);

		const FCyLandNeighborInfo* MapEntry = SceneProxyMap->FindRef(ComponentBase);
		if (MapEntry == this) //(/*ensure*/(MapEntry == this))
		{
			SceneProxyMap->Remove(ComponentBase);

			if (SceneProxyMap->Num() == 0)
			{
				// remove the entire CyLandKey entry as this is the last scene proxy
				SharedSceneProxyMap.Remove(CyLandKey);
			}
			else
			{
				// remove reference to us from our neighbors
				if (Neighbors[0])
				{
					Neighbors[0]->Neighbors[3] = nullptr;
				}
				if (Neighbors[1])
				{
					Neighbors[1]->Neighbors[2] = nullptr;
				}
				if (Neighbors[2])
				{
					Neighbors[2]->Neighbors[1] = nullptr;
				}
				if (Neighbors[3])
				{
					Neighbors[3]->Neighbors[0] = nullptr;
				}
			}
		}
	}
}

//
// FCyLandMeshProxySceneProxy
//
FCyLandMeshProxySceneProxy::FCyLandMeshProxySceneProxy(UStaticMeshComponent* InComponent, const FGuid& InGuid, const TArray<FIntPoint>& InProxyComponentBases, int8 InProxyLOD)
	: FStaticMeshSceneProxy(InComponent, false)
{
	if (!IsComponentLevelVisible())
	{
		bNeedsLevelAddedToWorldNotification = true;
	}

	ProxyNeighborInfos.Empty(InProxyComponentBases.Num());
	for (FIntPoint ComponentBase : InProxyComponentBases)
	{
		new(ProxyNeighborInfos) FCyLandNeighborInfo(InComponent->GetWorld(), InGuid, ComponentBase, nullptr, InProxyLOD, 0);
	}
}

SIZE_T FCyLandMeshProxySceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}


void FCyLandMeshProxySceneProxy::CreateRenderThreadResources()
{
	FStaticMeshSceneProxy::CreateRenderThreadResources();

	if (IsComponentLevelVisible())
	{
		for (FCyLandNeighborInfo& Info : ProxyNeighborInfos)
		{
			Info.RegisterNeighbors();
		}
	}
}

void FCyLandMeshProxySceneProxy::OnLevelAddedToWorld()
{
	for (FCyLandNeighborInfo& Info : ProxyNeighborInfos)
	{
		Info.RegisterNeighbors();
	}
}

FCyLandMeshProxySceneProxy::~FCyLandMeshProxySceneProxy()
{
	for (FCyLandNeighborInfo& Info : ProxyNeighborInfos)
	{
		Info.UnregisterNeighbors();
	}
}

FPrimitiveSceneProxy* UCyLandMeshProxyComponent::CreateSceneProxy()
{
	if (GetStaticMesh() == NULL
		|| GetStaticMesh()->RenderData == NULL
		|| GetStaticMesh()->RenderData->LODResources.Num() == 0
		|| GetStaticMesh()->RenderData->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() == 0)
	{
		return NULL;
	}

	return new FCyLandMeshProxySceneProxy(this, CyLandGuid, ProxyComponentBases, ProxyLOD);
}
