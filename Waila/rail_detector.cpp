#include "rail_detector.h"
#include "plugin_helpers.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"

#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace SDK;

namespace Waila
{
	namespace
	{
		// Bounds for the network scan. Both stop a runaway fill and set
		// RailInfo::truncated when they bite.
		constexpr size_t kMaxNodes   = 256;
		constexpr size_t kMaxSources = 64;

		// How far from a network node a building can sit and still count as fed by
		// it, in unreal units (1200 = 12 m). INFERRED: the SDK has no explicit link
		// between a building and a drone path point, so proximity stands in for it,
		// the way cargo_sender_detector matches a sender by distance.
		constexpr double kAttachRadius = 1200.0;

		constexpr std::chrono::milliseconds kRefresh{ 1000 };

		// One item's running totals while the sources are summed.
		struct Tally
		{
			FWeakObjectPtr item;
			std::string    displayName;
			float          nowPerMin = 0.0f;
			float          maxPerMin = 0.0f;
			bool           known     = true;   // false once any source's rate could not be read
		};

		// Everything the card shows, kept between refreshes. Item data is held as a
		// weak pointer and resolved on use. `rail` is only ever compared, never
		// dereferenced: a recycled address could at worst show the previous rail's
		// numbers until the next refresh.
		struct Cache
		{
			const AActor*                         rail = nullptr;
			std::chrono::steady_clock::time_point stamp{};
			int32_t                               sourceCount = 0;
			bool                                  truncated   = false;
			std::vector<Tally>                    tallies;
		};

		Cache g_cache;

		// Items per minute of one crafter, from the recipe it is working on.
		// full = output count / BuildTime x CraftingSpeed x 60.
		// outItem is set whenever there is a recipe; the return value is false when
		// the rate itself can't be worked out.
		bool CrafterFullRate(ACrCrafter* crafter, UAuItemDataBase*& outItem, float& outFull)
		{
			outItem = nullptr;

			UCrCraftingComponent* comp = crafter->CraftComponent;
			if (!comp)
				return false;

			const TArray<FAuCraftItem>& items = comp->ItemsToCraft;
			if (items.Num() < 1 || !items[0].RecipeData)
				return false;

			UAuItemRecipeData* recipe = items[0].RecipeData;
			outItem = recipe->GetItemDataBase();

			const int32_t count = items[0].OutputItem.Count > 0 ? items[0].OutputItem.Count : recipe->OutputItem.Count;
			if (!outItem || count <= 0 || recipe->BuildTime <= 0.0f)
				return false;

			outFull = static_cast<float>(count) / recipe->BuildTime * crafter->CraftingSettings.CraftingSpeed * 60.0f;
			return true;
		}

		// Whether the crafter is producing right now. Any of these flags stalls it.
		// LowElectricity is left out: INFERRED to slow a machine rather than stop
		// it, and no slowdown factor is exposed.
		bool CrafterRunning(ACrCrafter* crafter)
		{
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

		// Flood fill over the rail segments, from the looked-at one outwards. Rails
		// meet at their end actors, so two rails are neighbours when they share one.
		// Fills outNodes with the position of every end actor reached.
		void CollectNetwork(UWorld* world, ACrDronePathPointConnection* start,
		                    std::vector<FVector>& outNodes, bool& outTruncated)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDronePathPointConnection::StaticClass(), &all);

			std::unordered_map<const AActor*, std::vector<ACrDronePathPointConnection*>> byEnd;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* rail = static_cast<ACrDronePathPointConnection*>(all[i]);
				if (!rail || !UKismetSystemLibrary::IsValid(rail))
					continue;
				if (rail->ConnectedStartActor) byEnd[rail->ConnectedStartActor].push_back(rail);
				if (rail->ConnectedEndActor)   byEnd[rail->ConnectedEndActor].push_back(rail);
			}

			std::unordered_set<const AActor*> seenNodes;
			std::unordered_set<const AActor*> seenRails{ start };
			std::vector<ACrDronePathPointConnection*> queue{ start };

			for (size_t head = 0; head < queue.size(); ++head)
			{
				ACrDronePathPointConnection* rail = queue[head];
				AActor* ends[2] = { rail->ConnectedStartActor, rail->ConnectedEndActor };

				for (AActor* node : ends)
				{
					if (!node || !seenNodes.insert(node).second)
						continue;

					if (seenNodes.size() > kMaxNodes)
					{
						outTruncated = true;
						return;
					}

					if (UKismetSystemLibrary::IsValid(node))
						outNodes.push_back(node->K2_GetActorLocation());

					auto it = byEnd.find(node);
					if (it == byEnd.end())
						continue;
					for (ACrDronePathPointConnection* next : it->second)
					{
						if (seenRails.insert(next).second)
							queue.push_back(next);
					}
				}
			}
		}

		// Crafters near the network, summed per output item. A rail with a filter
		// only counts the sources that make that item.
		void CollectSources(UWorld* world, const std::vector<FVector>& nodes,
		                    const std::string& filterUnique, Cache& out)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrCrafter::StaticClass(), &all);

			const double radiusSq = kAttachRadius * kAttachRadius;
			std::unordered_map<std::string, size_t> index;

			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* crafter = static_cast<ACrCrafter*>(all[i]);
				if (!crafter || !UKismetSystemLibrary::IsValid(crafter))
					continue;

				const FVector at = crafter->K2_GetActorLocation();
				bool attached = false;
				for (const FVector& n : nodes)
				{
					const double dx = at.X - n.X, dy = at.Y - n.Y, dz = at.Z - n.Z;
					if (dx * dx + dy * dy + dz * dz <= radiusSq)
					{
						attached = true;
						break;
					}
				}
				if (!attached)
					continue;

				UAuItemDataBase* item = nullptr;
				float full = 0.0f;
				const bool readable = CrafterFullRate(crafter, item, full);

				// No recipe means no item to attribute anything to.
				if (!item)
					continue;

				const std::string key = item->UniqueItemName.ToString();
				if (!filterUnique.empty() && key != filterUnique)
					continue;

				if (out.sourceCount >= static_cast<int32_t>(kMaxSources))
				{
					out.truncated = true;
					break;
				}
				++out.sourceCount;

				auto found = index.find(key);
				if (found == index.end())
				{
					found = index.emplace(key, out.tallies.size()).first;
					Tally t;
					t.item.ObjectIndex        = item->Index;
					t.item.ObjectSerialNumber = 0;
					t.displayName = UKismetTextLibrary::Conv_TextToString(item->ItemName).ToString();
					out.tallies.push_back(std::move(t));
				}
				Tally& t = out.tallies[found->second];

				if (!readable)
				{
					t.known = false;
					continue;
				}

				t.maxPerMin += full;
				if (CrafterRunning(crafter))
					t.nowPerMin += full;
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

		const auto now = std::chrono::steady_clock::now();
		if (g_cache.rail != actor || now - g_cache.stamp >= kRefresh)
		{
			UWorld* world = UWorld::GetWorld();
			if (!world)
				return true;

			g_cache = Cache{};
			g_cache.rail  = actor;
			g_cache.stamp = now;

			std::vector<FVector> nodes;
			CollectNetwork(world, rail, nodes, g_cache.truncated);
			CollectSources(world, nodes, filterUnique, g_cache);
		}

		outInfo.sourceCount = g_cache.sourceCount;
		outInfo.truncated   = g_cache.truncated;

		for (const Tally& t : g_cache.tallies)
		{
			RailRateEntry entry;
			entry.displayName = t.displayName;

			// A dead item just loses its icon.
			UObject* obj = t.item.Get();
			if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
				entry.itemData = static_cast<UAuItemDataBase*>(obj);

			if (t.known)
			{
				entry.perMinMax = t.maxPerMin;
				entry.perMinNow = t.nowPerMin;
			}
			outInfo.rates.push_back(std::move(entry));
		}

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
	}
}
