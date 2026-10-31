#include "rail_detector.h"
#include "rail_network.h"
#include "plugin_helpers.h"
#include "waila_functions.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SDK;

namespace Waila
{
	namespace
	{
		// Bounds for the source walk. Both stop a runaway walk and set
		// RailInfo::truncated when they bite.
		constexpr size_t kMaxRails   = 1024;
		constexpr size_t kMaxSources = 256;

		// Only actors the game has spawned are visible to GetAllActorsOfClass, and
		// player buildings are streamed in around the camera (INFERRED), so a long
		// rail can be cut short without `truncated` being set.

		constexpr std::chrono::milliseconds kRefresh{ 1000 };

		// One item's running totals while the sources are summed.
		struct Tally
		{
			FWeakObjectPtr item;
			std::string    uniqueName;
			std::string    displayName;
			int32_t        sources   = 0;
			float          nowPerMin = 0.0f;
			float          maxPerMin = 0.0f;
			bool           maxKnown  = true;   // false once any source's full rate could not be read
			bool           nowKnown  = true;   // false once any source's state could not be read
		};

		// Everything the card shows for one rail, kept between refreshes. Item data
		// is held as a weak pointer and resolved on use. `rail` is only ever
		// compared, never dereferenced: a recycled address could at worst show the
		// previous rail's numbers until the next refresh.
		struct Cache
		{
			const AActor*                         rail = nullptr;
			std::chrono::steady_clock::time_point stamp{};
			int32_t                               sourceCount = 0;
			bool                                  truncated   = false;
			std::vector<Tally>                    tallies;
		};

		Cache g_cache;

		// Items per minute of one crafter, from the recipe it is working on, or for
		// an idle crafter the recipe it is set to.
		// full = output count / BuildTime x CraftingSpeed x 60.
		// CraftingSpeed is the live one from the crafting fragment, falling back to
		// the class default. CraftingMultiplier is left out (INFERRED: it sets how
		// many are requested, not how fast they are made).
		// outItem is set whenever there is a recipe; the return value is false when
		// the rate itself can't be worked out.
		bool CrafterFullRate(ACrCrafter* crafter, UAuItemDataBase*& outItem, float& outFull)
		{
			outItem = nullptr;

			UCrCraftingComponent* comp = crafter->CraftComponent;
			if (!comp)
				return false;

			const FCrCraftingFragment* frag = nullptr;
			if (auto fnGetFrag = Waila::Functions::GetCraftingFragment())
				frag = fnGetFrag(comp);

			// ItemsToCraft is empty while a crafter waits for input (seen on every
			// crafter just after a save loads); the fragment still holds the recipe
			// the player picked (INFERRED).
			const TArray<FAuCraftItem>& items = comp->ItemsToCraft;
			const bool queued = items.Num() > 0 && items[0].RecipeData;

			UAuItemRecipeData* recipe = queued ? items[0].RecipeData
									: frag     ? static_cast<UAuItemRecipeData*>(frag->SelectedRecipe)
												: nullptr;
			if (!recipe)
				return false;
			outItem = recipe->GetItemDataBase();

			const int32_t count = queued && items[0].OutputItem.Count > 0 ? items[0].OutputItem.Count : recipe->OutputItem.Count;
			if (!outItem || count <= 0 || recipe->BuildTime <= 0.0f)
				return false;

			const float speed = frag ? frag->CraftingSpeed : crafter->CraftingSettings.CraftingSpeed;
			if (!(speed > 0.0f) || !std::isfinite(speed))
				return false;

			outFull = static_cast<float>(count) / recipe->BuildTime * speed * 60.0f;
			return true;
		}

		// Whether the crafter is producing right now: the game's own building state
		// says it is working, and none of the stalling flags is set. Inefficient
		// counts as working, and LowElectricity is left out: INFERRED to slow a
		// machine rather than stop it, with no slowdown factor exposed.
		// Returns false in `known` when the state could not be read.
		bool CrafterRunning(ACrCrafter* crafter, bool& known)
		{
			const ECrBuildingState state = crafter->GetBuildingState();
			known = state != ECrBuildingState::Unknown;
			if (state != ECrBuildingState::Working && state != ECrBuildingState::Inefficient)
				return false;

			static constexpr ECrBuildingMalfunctionFlags kStalls[] = {
				ECrBuildingMalfunctionFlags::Disabled,
				ECrBuildingMalfunctionFlags::NoElectricity,
				ECrBuildingMalfunctionFlags::TemperatureOutOfRange,
				ECrBuildingMalfunctionFlags::MissingInputItems,
				ECrBuildingMalfunctionFlags::OutputFull,
				ECrBuildingMalfunctionFlags::UnavailableInputItem,
			};

			for (ECrBuildingMalfunctionFlags flag : kStalls)
			{
				if (crafter->GetBuildingStateFlag(flag))
					return false;
			}
			return true;
		}

		RailNetwork::Vec3 ToVec(const FVector& v) { return { v.X, v.Y, v.Z }; }

		// The buildings drones serve, fetched once per scan. The scene holds their
		// bounds and floor height at the same indices; the crafter pointers (null
		// for storage and the like) are only used inside that scan.
		std::vector<ACrCrafter*> GatherBuildings(UWorld* world, RailNetwork::Scene& scene)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDroneSupplySystemBuildingBase::StaticClass(), &all);

			std::vector<ACrCrafter*> crafters;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				AActor* building = all[i];
				if (!building || !UKismetSystemLibrary::IsValid(building))
					continue;

				ACrCrafter* crafter = building->IsA(ACrCrafter::StaticClass()) ? static_cast<ACrCrafter*>(building) : nullptr;

				FVector origin{}, extent{};
				building->GetActorBounds(false, &origin, &extent, false);
				scene.buildings.push_back({ ToVec(origin), ToVec(extent), building->K2_GetActorLocation().Z, crafter != nullptr });
				crafters.push_back(crafter);
			}
			return crafters;
		}

		// The rail sockets of every path point: poles, rail supports, junctions,
		// mergers and lanes.
		void GatherPathPoints(UWorld* world, RailNetwork::Scene& scene)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDronePathPoint::StaticClass(), &all);

			std::vector<RailNetwork::NamedSocket> named;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* point = static_cast<ACrDronePathPoint*>(all[i]);
				if (!point || !UKismetSystemLibrary::IsValid(point))
					continue;

				named.clear();
				const TArray<FBuildingSocketData>& sockets = point->SocketsData;
				for (int32_t s = 0; s < sockets.Num(); ++s)
				{
					if (sockets[s].Socket)
						named.push_back({ sockets[s].Socket->SocketName.ToString(), ToVec(sockets[s].WorldTransform.Translation) });
				}
				scene.AddPathPoint(named);
			}
		}

		// World positions of a rail's first and last spline point. False when the
		// spline is missing.
		bool RailEnds(ACrDronePathPointConnection* rail, RailNetwork::Rail& out)
		{
			USplineComponent* spline = rail->Spline;
			if (!spline || !UKismetSystemLibrary::IsValid(spline))
				return false;

			const int32_t points = spline->GetNumberOfSplinePoints();
			if (points < 2)
				return false;

			out.start = ToVec(spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
			out.end   = ToVec(spline->GetLocationAtSplinePoint(points - 1, ESplineCoordinateSpace::World));
			return true;
		}

		// Every rail with a spline. The pointers are only compared, at the same
		// indices as the scene's rails.
		std::vector<const AActor*> GatherRails(UWorld* world, RailNetwork::Scene& scene)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDronePathPointConnection::StaticClass(), &all);

			std::vector<const AActor*> rails;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* rail = static_cast<ACrDronePathPointConnection*>(all[i]);
				RailNetwork::Rail ends;
				if (!rail || !UKismetSystemLibrary::IsValid(rail) || !RailEnds(rail, ends))
					continue;

				scene.rails.push_back(ends);
				rails.push_back(rail);
			}
			return rails;
		}

		// The rail's source crafters, summed per output item. Storage and stations
		// hold stock but make nothing, so the walk passes through them instead.
		void CollectSources(const std::vector<ACrCrafter*>& crafters,
		                    const std::vector<size_t>& sources, Cache& out)
		{
			std::unordered_map<std::string, size_t> index;

			for (size_t source : sources)
			{
				ACrCrafter* crafter = crafters[source];

				UAuItemDataBase* item = nullptr;
				float full = 0.0f;
				const bool readable = CrafterFullRate(crafter, item, full);

				// No recipe means no item to attribute anything to.
				if (!item)
					continue;

				const std::string key = item->UniqueItemName.ToString();
				++out.sourceCount;

				auto found = index.find(key);
				if (found == index.end())
				{
					found = index.emplace(key, out.tallies.size()).first;
					Tally t;
					t.item.ObjectIndex        = item->Index;
					t.item.ObjectSerialNumber = 0;
					t.uniqueName  = key;
					t.displayName = UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
					out.tallies.push_back(std::move(t));
				}
				Tally& t = out.tallies[found->second];

				++t.sources;

				if (!readable)
				{
					t.maxKnown = false;
					t.nowKnown = false;
					continue;
				}

				t.maxPerMin += full;

				bool stateKnown = false;
				if (CrafterRunning(crafter, stateKnown))
					t.nowPerMin += full;
				else if (!stateKnown)
					t.nowKnown = false;
			}
		}
	}

	bool RailDetector::IsRail(AActor* actor)
	{
		if (!actor || !UKismetSystemLibrary::IsValid(actor))
			return false;

		return actor->IsA(ACrDronePathPointConnection::StaticClass());
	}

	bool RailDetector::GetRailInfo(AActor* actor, RailInfo& outInfo)
	{
		if (!IsRail(actor))
			return false;

		ACrDronePathPointConnection* rail = static_cast<ACrDronePathPointConnection*>(actor);

		UCrBuildingData* buildingData = (rail->PlacementData && rail->PlacementData->IsA(UCrBuildingData::StaticClass()))
			? static_cast<UCrBuildingData*>(rail->PlacementData)
			: nullptr;

		outInfo.buildingData = buildingData;
		if (buildingData)
		{
			outInfo.buildingName = UKismetTextLibrary::Conv_TextToString(buildingData->BuildingName).ToString();
			outInfo.buildingDesc = UKismetTextLibrary::Conv_TextToString(buildingData->BuildingDescription).ToString();
		}
		else
		{
			outInfo.buildingName = actor->Class ? actor->Class->GetName() : "Drone Rail";
		}

		// A null filter means the rail carries anything (INFERRED).
		const UAuItemDataBase* filter = rail->GetFilter();
		std::string filterUnique;
		if (filter && UKismetSystemLibrary::IsValid(filter))
		{
			outInfo.filterName = UKismetTextLibrary::Conv_TextToString(filter->ItemName).ToString();
			filterUnique       = filter->UniqueItemName.ToString();
		}

		// Rails are only compared by address here, never dereferenced. With actor
		// pooling an address can come back as a different rail, which shows the old
		// rail's numbers for at most one refresh.
		const auto now = std::chrono::steady_clock::now();
		if (g_cache.rail != actor || now - g_cache.stamp >= kRefresh)
		{
			UWorld* world = UWorld::GetWorld();
			if (!world)
				return true;

			g_cache = Cache{};
			g_cache.rail  = actor;
			g_cache.stamp = now;

			RailNetwork::Scene scene;
			const std::vector<const AActor*> rails    = GatherRails(world, scene);
			const std::vector<ACrCrafter*>   crafters = GatherBuildings(world, scene);
			GatherPathPoints(world, scene);

			const auto self = std::find(rails.begin(), rails.end(), actor);
			if (self != rails.end())
			{
				const RailNetwork::Graph   graph(scene);
				const RailNetwork::Sources sources = graph.Find(
					static_cast<size_t>(self - rails.begin()), kMaxRails, kMaxSources);
				g_cache.truncated = sources.truncated;
				CollectSources(crafters, sources.crafters, g_cache);
			}

			// Biggest flows first, so a cut-off list drops the smallest.
			std::stable_sort(g_cache.tallies.begin(), g_cache.tallies.end(),
				[](const Tally& a, const Tally& b) { return a.maxPerMin > b.maxPerMin; });
		}

		outInfo.truncated = g_cache.truncated;

		// A filtered rail only carries that item.
		for (const Tally& t : g_cache.tallies)
		{
			if (!filterUnique.empty() && t.uniqueName != filterUnique)
				continue;

			outInfo.sourceCount += t.sources;

			RailRateEntry entry;
			entry.displayName = t.displayName;

			// A dead item just loses its icon.
			UObject* obj = t.item.Get();
			if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
				entry.itemData = static_cast<UAuItemDataBase*>(obj);

			if (t.maxKnown)
				entry.perMinMax = t.maxPerMin;
			if (t.maxKnown && t.nowKnown)
				entry.perMinNow = t.nowPerMin;
			outInfo.rates.push_back(std::move(entry));
		}

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
	}
}
