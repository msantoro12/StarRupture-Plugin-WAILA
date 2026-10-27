#include "rail_detector.h"
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
#include <map>
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
		constexpr size_t kMaxSources = 256;

		// INFERRED, as is everything about which buildings a rail serves: the SDK
		// has no explicit building-to-rail link. ConnectedStartActor and
		// ConnectedEndActor are plain AActor*, and the rail mass spawner snaps rail
		// ends to a building's logistics socket, so an end actor is either a
		// building (counted directly, and the fill stops there) or a pole. Poles
		// also pick up crafters within kAttachRadius (unreal units, 1200 = 12 m),
		// the way cargo_sender_detector matches a sender by distance. A building
		// that only consumes from the rail cannot be told apart from one that feeds it.
		//
		// Only actors the game has spawned are visible to GetAllActorsOfClass, and
		// player buildings are streamed in around the camera (INFERRED), so a long
		// rail can be cut short without `truncated` being set.
		constexpr double kAttachRadius = 1200.0;

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

		// Everything the card shows, kept between refreshes. Item data is held as a
		// weak pointer and resolved on use. `rail` is only ever compared, never
		// dereferenced: a recycled address could at worst show the previous rail's
		// numbers until the next refresh.
		//
		// The tallies cover every item on the whole network, and `members` is the set
		// of rails the scan reached. Looking at another rail of the same network
		// reuses the cache, so sweeping the crosshair along a run does not rescan.
		struct Cache
		{
			std::unordered_set<const AActor*>     members;
			std::chrono::steady_clock::time_point stamp{};
			int32_t                               sourceCount = 0;
			bool                                  truncated   = false;
			std::vector<Tally>                    tallies;
		};

		Cache g_cache;

		// WailaRailProbe: temporary diagnostic. What the last scan saw, as text,
		// logged once each time the looked-at rail changes.
		std::string ProbeActor(AActor* actor)
		{
			if (!actor)
				return "null";
			if (!UKismetSystemLibrary::IsValid(actor))
				return "invalid";
			return (actor->Class ? actor->Class->GetName() : std::string("?")) + "/" + actor->GetName();
		}

		struct Probe
		{
			int32_t                    railsInWorld    = 0;
			int32_t                    craftersInWorld = 0;
			size_t                     railsVisited    = 0;
			std::map<std::string, int> nodeClasses;
			std::string                sources;
			int32_t                    attached        = 0;
			int32_t                    noRecipe        = 0;
			int32_t                    nullEnds        = 0;
			int32_t                    invalidEnds     = 0;

			// The log line is capped at 1 KB, so only the first few are named.
			void AddSource(AActor* crafter, bool atEnd, const std::string& what)
			{
				if (++attached <= 6)
					sources += " " + ProbeActor(crafter) + (atEnd ? "(end)=" : "(near)=") + what;
			}
		};

		Probe g_probe;

		// Rails here are only compared by address, never dereferenced. A line is
		// logged once the crosshair has rested on a rail for kProbeDwell, so a
		// junction that flips between two rails does not log every frame.
		constexpr std::chrono::milliseconds kProbeDwell{ 1000 };
		const AActor*                         g_probeRail   = nullptr;
		const AActor*                         g_probeLogged = nullptr;
		std::chrono::steady_clock::time_point g_probeSince{};

		// The crafter closest to a point, as "Class/Name@12.3m", whether or not it
		// is attached. Settles whether a rail end sits at a building with no actor.
		std::string ProbeNearestCrafter(const TArray<AActor*>& crafters, const FVector& at)
		{
			AActor* best = nullptr;
			double  bestSq = 0.0;
			for (int32_t i = 0; i < crafters.Num(); ++i)
			{
				AActor* c = crafters[i];
				if (!c || !UKismetSystemLibrary::IsValid(c))
					continue;
				const FVector p = c->K2_GetActorLocation();
				const double dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
				const double sq = dx * dx + dy * dy + dz * dz;
				if (!best || sq < bestSq)
				{
					best   = c;
					bestSq = sq;
				}
			}
			if (!best)
				return "none";

			char buf[32];
			snprintf(buf, sizeof(buf), "@%.1fm", std::sqrt(bestSq) / 100.0);
			return ProbeActor(best) + buf;
		}

		void ProbeLog(UWorld* world, ACrDronePathPointConnection* rail, const std::string& filter)
		{
			std::string nearStart = "?", nearEnd = "?";
			USplineComponent* spline = rail->Spline;
			if (world && spline && UKismetSystemLibrary::IsValid(spline))
			{
				const int32_t points = spline->GetNumberOfSplinePoints();
				if (points > 0)
				{
					TArray<AActor*> crafters{};
					UGameplayStatics::GetAllActorsOfClass(world, ACrCrafter::StaticClass(), &crafters);
					nearStart = ProbeNearestCrafter(crafters, spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
					nearEnd   = ProbeNearestCrafter(crafters, spline->GetLocationAtSplinePoint(points - 1, ESplineCoordinateSpace::World));
				}
			}

			std::string nodes;
			for (const auto& [cls, n] : g_probe.nodeClasses)
				nodes += " " + cls + "x" + std::to_string(n);

			LOG_INFO("WailaRailProbe: rail=%s filter=%s start=%s end=%s nearStart=%s nearEnd=%s | sources:%s | attached=%d noRecipe=%d crafters=%d | rails=%d visited=%zu nullEnds=%d invalidEnds=%d | nodes:%s",
				ProbeActor(rail).c_str(), filter.empty() ? "any" : filter.c_str(),
				ProbeActor(rail->ConnectedStartActor).c_str(), ProbeActor(rail->ConnectedEndActor).c_str(),
				nearStart.c_str(), nearEnd.c_str(),
				g_probe.sources.empty() ? " none" : g_probe.sources.c_str(),
				g_probe.attached, g_probe.noRecipe, g_probe.craftersInWorld,
				g_probe.railsInWorld, g_probe.railsVisited, g_probe.nullEnds, g_probe.invalidEnds,
				nodes.empty() ? " none" : nodes.c_str());
		}

		// Items per minute of one crafter, from the recipe it is working on.
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

			const TArray<FAuCraftItem>& items = comp->ItemsToCraft;
			if (items.Num() < 1 || !items[0].RecipeData)
				return false;

			UAuItemRecipeData* recipe = items[0].RecipeData;
			outItem = recipe->GetItemDataBase();

			const int32_t count = items[0].OutputItem.Count > 0 ? items[0].OutputItem.Count : recipe->OutputItem.Count;
			if (!outItem || count <= 0 || recipe->BuildTime <= 0.0f)
				return false;

			float speed = crafter->CraftingSettings.CraftingSpeed;
			if (auto fnGetFrag = Waila::Functions::GetCraftingFragment())
			{
				if (const FCrCraftingFragment* frag = fnGetFrag(comp))
					speed = frag->CraftingSpeed;
			}
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

		// Flood fill over the rail segments, from the looked-at one outwards. Rails
		// meet at their end actors, so two rails are neighbours when they share one.
		// A crafter at an end is a source and the fill does not continue through it,
		// or a factory's input and output rails would merge into one network.
		// Fills outPoles with the position of every other end actor reached,
		// outDirect with the crafters, and outRails with the rails visited.
		void CollectNetwork(UWorld* world, ACrDronePathPointConnection* start,
		                    std::vector<FVector>& outPoles, std::unordered_set<const AActor*>& outDirect,
		                    std::unordered_set<const AActor*>& outRails, bool& outTruncated)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDronePathPointConnection::StaticClass(), &all);

			g_probe.railsInWorld = all.Num();

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
			std::unordered_set<const AActor*>& seenRails = outRails;
			seenRails.insert(start);
			std::vector<ACrDronePathPointConnection*> queue{ start };

			for (size_t head = 0; head < queue.size(); ++head)
			{
				ACrDronePathPointConnection* rail = queue[head];
				AActor* ends[2] = { rail->ConnectedStartActor, rail->ConnectedEndActor };

				for (AActor* node : ends)
				{
					if (!node)
					{
						++g_probe.nullEnds;
						continue;
					}
					if (!seenNodes.insert(node).second)
						continue;

					if (seenNodes.size() > kMaxNodes)
					{
						outTruncated = true;
						return;
					}

					if (!UKismetSystemLibrary::IsValid(node))
					{
						++g_probe.invalidEnds;
						continue;
					}

					++g_probe.nodeClasses[node->Class ? node->Class->GetName() : "?"];

					if (node->IsA(ACrCrafter::StaticClass()))
					{
						outDirect.insert(node);
						continue;
					}
					outPoles.push_back(node->K2_GetActorLocation());

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

		// Crafters on the network, summed per output item. Stations, storage and
		// receivers hold stock but have no production rate, so they are not sources.
		void CollectSources(UWorld* world, const std::vector<FVector>& poles,
		                    const std::unordered_set<const AActor*>& direct, Cache& out)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrCrafter::StaticClass(), &all);

			g_probe.craftersInWorld = all.Num();

			const double radiusSq = kAttachRadius * kAttachRadius;
			std::unordered_map<std::string, size_t> index;

			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* crafter = static_cast<ACrCrafter*>(all[i]);
				if (!crafter || !UKismetSystemLibrary::IsValid(crafter))
					continue;

				const bool atEnd = direct.count(crafter) != 0;
				bool attached = atEnd;
				if (!attached)
				{
					const FVector at = crafter->K2_GetActorLocation();
					for (const FVector& n : poles)
					{
						const double dx = at.X - n.X, dy = at.Y - n.Y, dz = at.Z - n.Z;
						if (dx * dx + dy * dy + dz * dz <= radiusSq)
						{
							attached = true;
							break;
						}
					}
				}
				if (!attached)
					continue;

				UAuItemDataBase* item = nullptr;
				float full = 0.0f;
				const bool readable = CrafterFullRate(crafter, item, full);

				// No recipe means no item to attribute anything to.
				if (!item)
				{
					++g_probe.noRecipe;
					g_probe.AddSource(crafter, atEnd, "no-recipe");
					continue;
				}

				const std::string key = item->UniqueItemName.ToString();
				g_probe.AddSource(crafter, atEnd, key);

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
		// network's numbers for at most one refresh.
		const auto now = std::chrono::steady_clock::now();
		if (g_cache.members.count(actor) == 0 || now - g_cache.stamp >= kRefresh)
		{
			UWorld* world = UWorld::GetWorld();
			if (!world)
				return true;

			g_cache = Cache{};
			g_cache.stamp = now;
			g_probe = Probe{};

			std::vector<FVector> poles;
			std::unordered_set<const AActor*> direct;
			CollectNetwork(world, rail, poles, direct, g_cache.members, g_cache.truncated);
			CollectSources(world, poles, direct, g_cache);

			// Biggest flows first, so a cut-off list drops the smallest.
			std::stable_sort(g_cache.tallies.begin(), g_cache.tallies.end(),
				[](const Tally& a, const Tally& b) { return a.maxPerMin > b.maxPerMin; });

			g_probe.railsVisited = g_cache.members.size();
		}

		if (g_probeRail != actor)
		{
			g_probeRail  = actor;
			g_probeSince = now;
		}
		else if (g_probeLogged != actor && now - g_probeSince >= kProbeDwell)
		{
			g_probeLogged = actor;
			ProbeLog(UWorld::GetWorld(), rail, filterUnique);
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
		g_probe = Probe{};
		g_probeRail   = nullptr;
		g_probeLogged = nullptr;
	}
}
