#include "rail_detector.h"
#include "rail_network.h"
#include "rail_mass.h"
#include "crafter_detector.h"
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

		// Everything the rail layout found for one rail, kept between refreshes:
		// each loaded crafter's part in the flow of an item (rail_network.h,
		// SumFlows), and the items' names. Item data is held as a weak pointer and
		// resolved on use. `rail` is only ever compared, never dereferenced: a
		// recycled address could at worst show the previous rail's numbers until
		// the next refresh.
		struct ItemInfo
		{
			FWeakObjectPtr item;
			std::string    displayName;
		};

		struct Cache
		{
			const AActor*                             rail = nullptr;
			std::chrono::steady_clock::time_point     stamp{};
			bool                                      truncated = false;
			std::vector<RailNetwork::FlowPart>        parts;
			std::unordered_map<std::string, ItemInfo> items;
		};

		Cache g_cache;

		// The item rows: one per item, summed over every source and taker both
		// ways of looking found, rebuilt whenever either refreshes.
		std::vector<RailNetwork::ItemFlow> g_flows;

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

		// A building found through its actor and through the drones counts once in
		// the sums, so both name it by its Mass entity; without one (the Mass
		// reader off) the actor's address stands in, only ever compared.
		uint64_t BuildingKey(ACrCrafter* crafter)
		{
			const RailMass::Entity entity = RailMass::EntityOf(crafter);
			return entity ? entity : static_cast<uint64_t>(reinterpret_cast<uintptr_t>(crafter));
		}

		// What a recipe takes at a crafting speed, by item unique name: count /
		// BuildTime x speed x 60 a minute for each ingredient.
		struct Input
		{
			std::string item;
			float       perMin = 0.0f;
		};

		std::vector<Input> InputsOf(UAuItemRecipeData* recipe, float speed)
		{
			std::vector<Input> inputs;
			const Fields::StructArray needed(Fields::NeededResources.In(recipe), Fields::RecipeOrderSize);
			for (int32_t i = 0; i < needed.Num(); ++i)
			{
				const uint8_t* order = needed.At(i);
				UAuItemDataBase* item = Fields::ItemDefaultsOf(Fields::OrderItemClass(order));
				float rate = 0.0f;
				if (item && CountRate(recipe, Fields::ValueOr<int32_t>(Fields::OrderCount, order), speed, rate))
					inputs.push_back({ Fields::UniqueNameOf(item), rate });
			}
			return inputs;
		}

		// A crafter taking items off the line: one part per ingredient in `inputs`,
		// with whether it is asking the drones for it right now.
		void AddTaker(uint64_t key, RailMass::Entity entity, const std::vector<Input>& inputs,
		              std::vector<RailNetwork::FlowPart>& parts)
		{
			if (inputs.empty())
				return;

			RailMass::Requests wanted;
			RailMass::ReadRequests(entity, wanted);
			std::vector<std::string> wantedNames;
			for (UAuItemDataBase* want : wanted.wanted)
				wantedNames.push_back(Fields::UniqueNameOf(want));

			for (const Input& in : inputs)
			{
				RailNetwork::FlowPart taker{ key, in.item, false, true, in.perMin };
				taker.requestsKnown = wanted.known;
				taker.requesting    = std::find(wantedNames.begin(), wantedNames.end(), in.item) != wantedNames.end();
				parts.push_back(std::move(taker));
			}
		}

		// The crafters the rail layout finds: the ones feeding the stretch, at
		// their output with every one running, and the ones taking items off the
		// line ahead of it. A crafter with no recipe has no item to count.
		void CollectNetwork(const std::vector<ACrCrafter*>& crafters, const RailNetwork::Sources& sources,
		                    const RailNetwork::Consumers& consumers, Cache& out)
		{
			for (size_t source : sources.crafters)
			{
				ACrCrafter* crafter = crafters[source];
				UAuItemDataBase* item = nullptr;
				float full = 0.0f;
				const bool readable = CrafterFullRate(crafter, item, full);
				if (!item)
					continue;

				const std::string unique = Fields::UniqueNameOf(item);
				if (!out.items.count(unique))
				{
					ItemInfo info;
					info.item.ObjectIndex        = item->Index;
					info.item.ObjectSerialNumber = 0;
					info.displayName = Fields::DisplayNameOf(item);
					out.items.emplace(unique, std::move(info));
				}
				out.parts.push_back({ BuildingKey(crafter), unique, true, true, readable ? full : -1.0f });
			}

			for (size_t consumer : consumers.crafters)
			{
				ACrCrafter* crafter = crafters[consumer];
				CrafterWork work;
				if (crafter && CrafterWorkOf(crafter, work))
					AddTaker(BuildingKey(crafter), RailMass::EntityOf(crafter), InputsOf(work.recipe, work.speed), out.parts);
			}
		}

		// ---- From the game's Mass data (rail_mass.h) ---------------------------

		// The most buildings kept per rail: the reader's own cap on one observation.
		// Rows merge by building name, so this only bounds the work and the counts.
		constexpr size_t kMaxPeers = 256;

		// Stands in for a building the Mass data cannot name: the building names are
		// off on this build or faulted, the entity is gone or not built, it has no
		// building parameters, or they hold no building data (see ReadBuilding).
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
		bool                                  truncated = false;   // more buildings than kMaxPeers were seen
			int32_t                               drones    = 0;
			float                                 capacity  = -1.0f;   // items a minute; negative when unread
			std::vector<MassPeer>                 peers;
			std::vector<RailNetwork::FlowPart>    parts;    // each peer's part in the item sums, rebuilt each refresh
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

		// A crafter's rate for the item the rail moves, items a minute: what it
		// makes when the item is its product, what it takes when the item is an
		// ingredient of its recipe, 0 while it is stalled. Unread for any other
		// building (storage, stations) or when the recipe has neither. Also adds
		// the building's part to the item's sums, at its full rate: storage, a
		// station or a crafter handing out what it does not make is stock.
		void RateOf(MassPeer& peer, std::vector<RailNetwork::FlowPart>& parts)
		{
			peer.perMinNow = -1.0f;
			RailMass::CrafterState crafter;
			const bool isCrafter = RailMass::ReadCrafter(peer.entity, crafter) && crafter.isCrafter;
			const bool rates     = Features::Crafters.On();
			UAuItemRecipeData* recipe = isCrafter && rates ? crafter.recipe : nullptr;

			float full = -1.0f;
			if (recipe)
			{
				UAuItemDataBase* made = recipe->GetItemDataBase();
				if (peer.source && made && Fields::UniqueNameOf(made) == peer.itemUnique)
				{
					if (!RecipeRate(recipe, 0, crafter.speed, full))
						full = -1.0f;
					parts.push_back({ peer.entity, peer.itemUnique, true, true, full });
				}
				else
				{
					std::vector<Input> inputs = InputsOf(recipe, crafter.speed);
					inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
						[&](const Input& in) { return in.item != peer.itemUnique; }), inputs.end());
					if (!inputs.empty())
						full = inputs.front().perMin;
					if (!peer.source)
						AddTaker(peer.entity, peer.entity, inputs, parts);
				}
			}
			if (full >= 0.0f)
				peer.perMinNow = crafter.stalled ? 0.0f : full;

			const bool counted = std::any_of(parts.begin(), parts.end(), [&](const RailNetwork::FlowPart& p)
				{ return p.building == peer.entity && p.source == peer.source && p.item == peer.itemUnique; });
			if (!counted)
				parts.push_back({ peer.entity, peer.itemUnique, peer.source, isCrafter && !rates });
		}

		void Merge(MassCache& cache, const RailMass::Observation& seen)
		{
			for (const RailMass::Peer& p : seen.peers)
			{
				const std::string unique = p.item ? Fields::UniqueNameOf(p.item) : std::string();
				const auto same = std::find_if(cache.peers.begin(), cache.peers.end(), [&](const MassPeer& m)
					{ return m.entity == p.entity && m.source == p.source && m.itemUnique == unique; });
				if (same != cache.peers.end())
					continue;
				if (cache.peers.size() >= kMaxPeers)
				{
					cache.truncated = true;
					continue;
				}

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

			cache.parts.clear();
			for (MassPeer& peer : cache.peers)
			{
				if (peer.buildingName.empty())
					peer.buildingName = BuildingNameOf(peer.entity);
				if (!peer.itemUnique.empty())
					RateOf(peer, cache.parts);
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
		bool massRefreshed = false;

		// The rail's ends from the Mass data, listed under the item rates below.
		if (Features::ShowRailSources())
		{
			if (g_massCache.rail != actor || now - g_massCache.stamp >= kRefresh)
			{
				if (g_massCache.rail != actor)
					g_massCache = MassCache{};
				g_massCache.rail  = actor;
				g_massCache.stamp = now;
				RefreshMass(actor, g_massCache);
				massRefreshed = true;
			}

			outInfo.fromMass = true;
			outInfo.peersTruncated = g_massCache.truncated;

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

		bool refreshed = false;
		if (Features::ShowDroneRails() && (g_cache.rail != actor || now - g_cache.stamp >= kRefresh))
		{
			UWorld* world = PluginSdkRuntime::GetWorld();
			if (world)
			{
				g_cache = Cache{};
				g_cache.rail  = actor;
				g_cache.stamp = now;
				refreshed     = true;

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
					CollectNetwork(crafters, sources, consumers, g_cache);
				}
			}
		}

		// One row per item: the layout's parts first (its sources carry the queued
		// output count), then the drones', each building counted once. A filtered
		// rail only carries that item.
		if (refreshed || massRefreshed)
		{
			std::vector<RailNetwork::FlowPart> parts;
			if (Features::ShowDroneRails() && g_cache.rail == actor)
				parts = g_cache.parts;
			if (Features::ShowRailSources() && g_massCache.rail == actor)
				parts.insert(parts.end(), g_massCache.parts.begin(), g_massCache.parts.end());
			if (!filterUnique.empty())
				parts.erase(std::remove_if(parts.begin(), parts.end(),
					[&](const RailNetwork::FlowPart& p) { return p.item != filterUnique; }), parts.end());

			const float capacity = Features::ShowRailCapacity() && g_massCache.rail == actor ? g_massCache.capacity : -1.0f;
			g_flows = RailNetwork::SumFlows(parts, capacity, Features::ShowRailRequests());
		}
		else if (g_cache.rail != actor && g_massCache.rail != actor)
			g_flows.clear();

		outInfo.truncated = g_cache.rail == actor && g_cache.truncated;
		for (const RailNetwork::ItemFlow& flow : g_flows)
		{
			RailRateEntry entry;
			entry.perMinMax = flow.max;
			entry.perMinMin = flow.min;
			entry.perMinNow = flow.now;
			outInfo.sourceCount += flow.sources;

			// The name from the layout, or else from a drone that carried it. A dead
			// item just loses its icon.
			FWeakObjectPtr item{};
			const auto info = g_cache.items.find(flow.item);
			if (info != g_cache.items.end())
			{
				entry.displayName = info->second.displayName;
				item              = info->second.item;
			}
			else
			{
				const auto peer = std::find_if(g_massCache.peers.begin(), g_massCache.peers.end(),
					[&](const MassPeer& p) { return p.itemUnique == flow.item; });
				if (peer != g_massCache.peers.end())
				{
					entry.displayName = peer->itemDisplay;
					item              = peer->item;
				}
			}
			UObject* obj = item.Get();
			if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
				entry.itemData = static_cast<UAuItemDataBase*>(obj);
			outInfo.rates.push_back(std::move(entry));
		}

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
		g_massCache = MassCache{};
		g_flows.clear();
	}
}
