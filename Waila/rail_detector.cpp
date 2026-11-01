#include "rail_detector.h"
#include "rail_network.h"
#include "rail_mass.h"
#include "plugin_helpers.h"
#include "waila_functions.h"
#include "waila_fields.h"
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

		// One item's totals while the sources and the consumers are summed.
		struct Tally
		{
			FWeakObjectPtr item;
			std::string    uniqueName;
			std::string    displayName;
			int32_t        sources    = 0;
			float          fullPerMin = 0.0f;   // with every source running; orders the list
			bool           fullKnown  = true;   // false once any source's rate could not be read

			// What the machines taking this item off the line ahead of the rail use,
			// in items a minute: all of them running full out, and just the ones
			// asking the drones for it right now.
			float          inputFullPerMin      = 0.0f;
			float          inputRequestedPerMin = 0.0f;
			bool           requestsKnown        = true;   // false once any such machine's requests could not be read
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

		// Items per minute of count items per craft of a recipe at a crafting
		// speed: count / BuildTime x speed x 60.
		bool CountRate(UAuItemRecipeData* recipe, int32_t count, float speed, float& outRate)
		{
			const float buildTime = Fields::ValueOr<float>(Fields::RecipeBuildTime, recipe);
			if (count <= 0 || buildTime <= 0.0f || !(speed > 0.0f) || !std::isfinite(speed))
				return false;

			outRate = static_cast<float>(count) / buildTime * speed * 60.0f;
			return true;
		}

		// Items per minute of one recipe at a crafting speed: output count /
		// BuildTime x speed x 60. countOverride (when above 0) replaces the
		// recipe's own output count.
		bool RecipeRate(UAuItemRecipeData* recipe, int32_t countOverride, float speed, float& outFull)
		{
			return CountRate(recipe, countOverride > 0 ? countOverride : Fields::RecipeOutputCount(recipe), speed, outFull);
		}

		// What a crafter works on: the recipe it is making, or for an idle crafter
		// the recipe it is set to, the output count of its queue entry (0 when it
		// has none) and its crafting speed. CraftingSpeed is the live one from the
		// crafting fragment, falling back to the class default. CraftingMultiplier
		// is left out (INFERRED: it sets how many are requested, not how fast they
		// are made). False when there is no recipe.
		struct CrafterWork
		{
			UAuItemRecipeData* recipe      = nullptr;
			int32_t            outputCount = 0;
			float              speed       = 0.0f;
		};

		bool CrafterWorkOf(ACrCrafter* crafter, CrafterWork& out)
		{
			UCrCraftingComponent* comp = Fields::ValueOr<UCrCraftingComponent*>(Fields::CraftComponent, crafter, nullptr);
			if (!comp)
				return false;

			const FCrCraftingFragment* frag = nullptr;
			if (auto fnGetFrag = Waila::Functions::GetCraftingFragment())
				frag = fnGetFrag(comp);

			// ItemsToCraft is empty while a crafter waits for input (seen on every
			// crafter just after a save loads); the fragment still holds the recipe
			// the player picked (INFERRED).
			const Fields::QueuedCraft first = Fields::FirstQueuedCraft(comp);
			const bool queued = first.recipe != nullptr;

			UAuItemRecipeData* recipe = queued ? first.recipe
									: frag     ? static_cast<UAuItemRecipeData*>(Fields::ValueOr<UCrItemRecipeData*>(Fields::FragSelected, frag, nullptr))
												: nullptr;
			if (!recipe)
				return false;

			const FCrCraftingSettings* settings = Fields::CraftingSettings.In(crafter);
			out.recipe      = recipe;
			out.outputCount = queued ? first.outputCount : 0;
			out.speed       = frag ? Fields::ValueOr<float>(Fields::FragSpeed, frag)
			                       : Fields::ValueOr<float>(Fields::SettingsSpeed, settings);
			return true;
		}

		// Items per minute of one crafter: output count / BuildTime x CraftingSpeed
		// x 60. outItem is set whenever there is a recipe; the return value is
		// false when the rate itself can't be worked out.
		bool CrafterFullRate(ACrCrafter* crafter, UAuItemDataBase*& outItem, float& outFull)
		{
			outItem = nullptr;

			CrafterWork work;
			if (!CrafterWorkOf(crafter, work))
				return false;
			outItem = work.recipe->GetItemDataBase();
			if (!outItem)
				return false;
			return RecipeRate(work.recipe, work.outputCount, work.speed, outFull);
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
				const Fields::StructArray sockets(Fields::SocketsData.In(point), Fields::SocketDataSize);
				for (int32_t s = 0; s < sockets.Num(); ++s)
				{
					const uint8_t* socket = sockets.At(s);
					const UStaticMeshSocket* mesh = Fields::ValueOr<UStaticMeshSocket*>(Fields::SocketRef, socket, nullptr);
					const FTransform* transform = Fields::SocketTransform.In(socket);
					if (mesh && transform)
						named.push_back({ mesh->SocketName.ToString(), ToVec(transform->Translation) });
				}
				scene.AddPathPoint(named);
			}
		}

		// World positions of a rail's first and last spline point. False when the
		// spline is missing.
		bool RailEnds(ACrDronePathPointConnection* rail, RailNetwork::Rail& out)
		{
			USplineComponent* spline = Fields::ValueOr<USplineComponent*>(Fields::Spline, rail, nullptr);
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

				const std::string key = Fields::UniqueNameOf(item);
				++out.sourceCount;

				auto found = index.find(key);
				if (found == index.end())
				{
					found = index.emplace(key, out.tallies.size()).first;
					Tally t;
					t.item.ObjectIndex        = item->Index;
					t.item.ObjectSerialNumber = 0;
					t.uniqueName  = key;
					t.displayName = Fields::DisplayNameOf(item);
					out.tallies.push_back(std::move(t));
				}
				Tally& t = out.tallies[found->second];

				++t.sources;

				if (readable)
					t.fullPerMin += full;
				else
					t.fullKnown = false;
			}
		}

		// The machines that take items off the line ahead of the rail, added to the
		// totals of the items the rail carries. A machine takes each ingredient of
		// its recipe at count / BuildTime x speed x 60 a minute; that goes into the
		// full figure, and into the requested one when the machine is asking the
		// drones for the item. A machine with no recipe takes nothing.
		void CollectConsumers(const std::vector<ACrCrafter*>& crafters,
		                      const std::vector<size_t>& consumers, Cache& out)
		{
			for (size_t consumer : consumers)
			{
				ACrCrafter* crafter = crafters[consumer];
				CrafterWork work;
				if (!crafter || !CrafterWorkOf(crafter, work))
					continue;

				bool                     asked = false;
				RailMass::Requests       wanted;
				std::vector<std::string> wantedNames;

				const Fields::StructArray needed(Fields::NeededResources.In(work.recipe), Fields::RecipeOrderSize);
				for (int32_t i = 0; i < needed.Num(); ++i)
				{
					const uint8_t* order = needed.At(i);
					UAuItemDataBase* item = Fields::ItemDefaultsOf(Fields::OrderItemClass(order));
					if (!item)
						continue;

					const std::string key = Fields::UniqueNameOf(item);
					const auto tally = std::find_if(out.tallies.begin(), out.tallies.end(),
						[&](const Tally& t) { return t.uniqueName == key; });
					float rate = 0.0f;
					if (tally == out.tallies.end()
						|| !CountRate(work.recipe, Fields::ValueOr<int32_t>(Fields::OrderCount, order), work.speed, rate))
						continue;
					tally->inputFullPerMin += rate;

					if (!asked)
					{
						asked = true;
						RailMass::ReadRequests(RailMass::EntityOf(crafter), wanted);
						for (UAuItemDataBase* want : wanted.wanted)
							wantedNames.push_back(Fields::UniqueNameOf(want));
					}
					if (!wanted.known)
						tally->requestsKnown = false;
					else if (std::find(wantedNames.begin(), wantedNames.end(), key) != wantedNames.end())
						tally->inputRequestedPerMin += rate;
				}
			}
		}

		// ---- From the game's Mass data (rail_mass.h) ---------------------------

		// More than this many distinct buildings stops being a glance.
		constexpr size_t kMaxPeers = 32;

		// Stands in for a building the Mass data cannot name: its entity is gone,
		// has no building parameters, or they hold no building data.
		const std::string kUnknownBuilding = "Unknown building";

		// One building and one item, seen at an end of the rail's traffic. Kept
		// between refreshes while the player keeps aiming at the rail, so a rail
		// whose drones come and go still lists everything seen on it. Item data
		// is a weak pointer, resolved on use.
		struct MassPeer
		{
			RailMass::Entity entity = 0;
			bool             source = false;
			FWeakObjectPtr   item;
			std::string      itemUnique;
			std::string      itemDisplay;
			std::string      buildingName;   // empty until its building data could be read
			float            perMinNow = -1.0f;
		};

		struct MassCache
		{
			const AActor*                         rail = nullptr;
			std::chrono::steady_clock::time_point stamp{};
			bool                                  railFound = false;
			int32_t                               drones    = 0;
			float                                 capacity  = -1.0f;   // items a minute; negative when unread
			std::vector<MassPeer>                 peers;
		};

		MassCache g_massCache;

		// The building's name from its Mass entity's building parameters, which
		// every entity keeps whether or not its actor is loaded. Empty when the
		// entity is gone or has no building data to name it.
		std::string BuildingNameOf(RailMass::Entity entity)
		{
			SDK::UCrBuildingData* data = nullptr;
			std::string name, desc;
			if (RailMass::ReadBuilding(entity, data) && Fields::ReadBuildingTexts(data, name, desc))
				return name;
			return std::string();
		}

		// A crafter's rate for the item the rail moves: only when it makes that item.
		void RateOf(MassPeer& peer)
		{
			peer.perMinNow = -1.0f;
			RailMass::CrafterState crafter;
			if (!Features::Crafters.On() || !RailMass::ReadCrafter(peer.entity, crafter) || !crafter.recipe)
				return;

			UAuItemDataBase* made = crafter.recipe->GetItemDataBase();
			float full = 0.0f;
			if (!made || Fields::UniqueNameOf(made) != peer.itemUnique
				|| !RecipeRate(crafter.recipe, 0, crafter.speed, full))
				return;
			peer.perMinNow = crafter.stalled ? 0.0f : full;
		}

		void Merge(MassCache& cache, const RailMass::Observation& seen)
		{
			for (const RailMass::Peer& p : seen.peers)
			{
				const std::string unique = p.item ? Fields::UniqueNameOf(p.item) : std::string();
				const auto same = std::find_if(cache.peers.begin(), cache.peers.end(), [&](const MassPeer& m)
					{ return m.entity == p.entity && m.source == p.source && m.itemUnique == unique; });
				if (same != cache.peers.end() || cache.peers.size() >= kMaxPeers)
					continue;

				MassPeer peer;
				peer.entity = p.entity;
				peer.source = p.source;
				if (p.item)
				{
					peer.item.ObjectIndex        = p.item->Index;
					peer.item.ObjectSerialNumber = 0;
					peer.itemUnique  = unique;
					peer.itemDisplay = Fields::DisplayNameOf(p.item);
				}
				cache.peers.push_back(std::move(peer));
			}
		}

		void RefreshMass(AActor* rail, MassCache& cache)
		{
			RailMass::Observation seen;
			const RailMass::Entity entity = RailMass::EntityOf(rail);
			if (entity && RailMass::Observe(entity, seen))
			{
				cache.railFound = seen.railFound;
				cache.drones    = seen.drones;
				Merge(cache, seen);
				RailMass::ReadCapacity(entity, cache.capacity);
			}

			for (MassPeer& peer : cache.peers)
			{
				if (peer.buildingName.empty())
					peer.buildingName = BuildingNameOf(peer.entity);
				RateOf(peer);
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

		UCrBuildingData* buildingData = Fields::BuildingDataOf(rail);

		outInfo.buildingData = buildingData;
		if (!Fields::ReadBuildingTexts(buildingData, outInfo.buildingName, outInfo.buildingDesc))
		{
			outInfo.buildingName = actor->Class ? actor->Class->GetName() : "Drone Rail";
		}

		// A null filter means the rail carries anything (INFERRED).
		const UAuItemDataBase* filter = rail->GetFilter();
		std::string filterUnique;
		if (filter && UKismetSystemLibrary::IsValid(filter))
		{
			outInfo.filterName = Fields::DisplayNameOf(filter);
			filterUnique       = Fields::UniqueNameOf(filter);
		}

		// Rails are only compared by address here, never dereferenced. With actor
		// pooling an address can come back as a different rail, which shows the old
		// rail's numbers for at most one refresh.
		const auto now = std::chrono::steady_clock::now();

		// The rail's ends from the Mass data, listed under the item rates below.
		if (Features::ShowRailSources())
		{
			if (g_massCache.rail != actor || now - g_massCache.stamp >= kRefresh)
			{
				UWorld* world = PluginSdkRuntime::GetWorld();
				if (g_massCache.rail != actor)
					g_massCache = MassCache{};
				g_massCache.rail  = actor;
				g_massCache.stamp = now;
				if (world)
					RefreshMass(actor, g_massCache);
			}

			outInfo.fromMass = true;

			// One row per end, building and item: buildings of the same name making
			// or taking the same item merge into one row, whichever entities they
			// are. rowKeys holds each row's key at the same index as outInfo.peers.
			std::vector<std::string> rowKeys;
			for (const MassPeer& peer : g_massCache.peers)
			{
				// A filtered rail only carries that item.
				if (!filterUnique.empty() && peer.itemUnique != filterUnique)
					continue;

				// Only a building the Mass data cannot name stays unnamed: its entity
				// is gone, or the game keeps no building data for it.
				const std::string& name = peer.buildingName.empty() ? kUnknownBuilding : peer.buildingName;
				++(peer.source ? outInfo.fromCount : outInfo.destCount);

				const std::string key = (peer.source ? "from|" : "to|") + name + "|" + peer.itemUnique;
				const auto same = std::find(rowKeys.begin(), rowKeys.end(), key);
				if (same != rowKeys.end())
				{
					RailPeerEntry& merged = outInfo.peers[static_cast<size_t>(same - rowKeys.begin())];
					++merged.count;
					if (peer.perMinNow >= 0.0f)
						merged.perMinNow = (std::max)(merged.perMinNow, 0.0f) + peer.perMinNow;
					continue;
				}

				RailPeerEntry entry;
				entry.source       = peer.source;
				entry.buildingName = name;
				entry.itemName     = peer.itemDisplay;
				entry.perMinNow    = peer.perMinNow;

				// A dead item just loses its icon.
				UObject* obj = peer.item.Get();
				if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
					entry.itemData = static_cast<UAuItemDataBase*>(obj);

				rowKeys.push_back(key);
				outInfo.peers.push_back(std::move(entry));
			}
			// Sources first.
			std::stable_partition(outInfo.peers.begin(), outInfo.peers.end(),
				[](const RailPeerEntry& e) { return e.source; });

			if (outInfo.peers.empty())
				outInfo.note = g_massCache.railFound ? "No cargo moving" : "Rail data not available";
		}

		if (!Features::ShowDroneRails())
			return true;

		if (g_cache.rail != actor || now - g_cache.stamp >= kRefresh)
		{
			UWorld* world = PluginSdkRuntime::GetWorld();
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
				const RailNetwork::Consumers consumers = graph.FindConsumers(sources, kMaxRails, kMaxSources);
				g_cache.truncated = sources.truncated || consumers.truncated;
				CollectSources(crafters, sources.crafters, g_cache);
				CollectConsumers(crafters, consumers.crafters, g_cache);
			}

			// Biggest flows first, so a cut-off list drops the smallest.
			std::stable_sort(g_cache.tallies.begin(), g_cache.tallies.end(),
				[](const Tally& a, const Tally& b) { return a.fullPerMin > b.fullPerMin; });
		}

		outInfo.truncated = g_cache.truncated;

		// A drone carries one item, so the rail's capacity is the same for each.
		const float capacity = Features::ShowRailCapacity() && g_massCache.rail == actor ? g_massCache.capacity : -1.0f;
		const bool  capped   = capacity >= 0.0f;
		const bool  asking   = Features::ShowRailRequests();

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

			// Three potentials, items a minute. The sources' output is capped by what
			// the rail carries. The machines ahead of this stretch take their share:
			// min is what is left with all of them running full out, current what is
			// left with just the ones asking for the item now. Without the capacity
			// there is no max, and the other two come from the sources alone.
			if (t.fullKnown)
			{
				const float supply = capped ? (std::min)(t.fullPerMin, capacity) : t.fullPerMin;
				if (capped)
					entry.perMinMax = supply;
				entry.perMinMin = (std::max)(0.0f, supply - t.inputFullPerMin);
				if (asking && t.requestsKnown)
					entry.perMinNow = (std::max)(0.0f, supply - t.inputRequestedPerMin);
			}
			outInfo.rates.push_back(std::move(entry));
		}

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
		g_massCache = MassCache{};
	}
}
