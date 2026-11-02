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
#include <unordered_set>
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

		// Stands in for a building the game's data cannot name: the building names
		// are off on this build or faulted, the entity is gone or not built, it has
		// no building parameters, or they hold no building data (see ReadBuilding).
		const std::string kUnknownBuilding = "Unknown building";

		// A loaded building's name, from its actor.
		std::string NameOf(ACrCrafter* crafter)
		{
			std::string name, desc;
			if (Fields::ReadBuildingTexts(Fields::BuildingDataOf(crafter), name, desc) && !name.empty())
				return name;
			return kUnknownBuilding;
		}

		// A crafter's rate right now from its full rate: 0 while its Mass data says
		// it is stalled (missing items or output full). Unread without the Mass
		// reader or a rate.
		float NowOf(RailMass::Entity entity, float full)
		{
			RailMass::CrafterState state;
			if (full < 0.0f || !entity || !RailMass::ReadCrafter(entity, state) || !state.isCrafter)
				return -1.0f;
			return state.stalled ? 0.0f : full;
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

		// A crafter taking items off the line: one part per ingredient in `inputs`.
		// `stalled` is negative when unread.
		void AddTaker(uint64_t key, const std::string& name, const std::vector<Input>& inputs, int stalled,
		              std::vector<RailNetwork::FlowPart>& parts)
		{
			for (const Input& in : inputs)
			{
				RailNetwork::FlowPart taker{ key, in.item, false, true, in.perMin };
				taker.now  = stalled < 0 ? -1.0f : stalled ? 0.0f : in.perMin;
				taker.name = name;
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
				RailNetwork::FlowPart part{ BuildingKey(crafter), unique, true, true, readable ? full : -1.0f };
				part.now  = NowOf(RailMass::EntityOf(crafter), part.full);
				part.name = NameOf(crafter);
				out.parts.push_back(std::move(part));
			}

			for (size_t consumer : consumers.crafters)
			{
				ACrCrafter* crafter = crafters[consumer];
				CrafterWork work;
				if (!crafter || !CrafterWorkOf(crafter, work))
					continue;
				RailMass::CrafterState state;
				const RailMass::Entity entity = RailMass::EntityOf(crafter);
				const int stalled = entity && RailMass::ReadCrafter(entity, state) && state.isCrafter ? state.stalled : -1;
				AddTaker(BuildingKey(crafter), NameOf(crafter), InputsOf(work.recipe, work.speed), stalled, out.parts);
			}
		}

		// ---- From the game's Mass data (rail_mass.h) ---------------------------

		// The most buildings kept per rail: the reader's own cap on one observation.
		// Lines merge by building name, so this only bounds the work and the counts.
		constexpr size_t kMaxPeers = 256;

		// One building and one item, seen at an end of the rail's traffic. Kept
		// while the rail is watched, so a rail whose drones come and go still
		// lists everything seen on it.
		struct MassPeer
		{
			RailMass::Entity entity = 0;
			bool             source = false;
			std::string      itemUnique;
			std::string      buildingName;   // empty until its building data could be read
			bool             carried   = false;   // seen on a drone carrying the item, not one on its way to pick it up
		};

		// A rail being watched: every rail aimed at in the last kWatchFor, by its
		// Mass entity (index and serial, checked by every read), loaded or not.
		// Sampled on the game tick every kSampleEvery whether or not its card is
		// up: the ends of every drone seen on it are kept, and the meter counts the
		// items that cross it. Items are held as weak pointers, resolved on use.
		constexpr std::chrono::milliseconds kSampleEvery{ 200 };
		constexpr std::chrono::seconds      kWatchFor{ 90 };
		constexpr size_t                    kMaxWatches = 4;

		struct Watch
		{
			RailMass::Entity                          rail = 0;
			std::chrono::steady_clock::time_point     aimed{};
			std::chrono::steady_clock::time_point     sampled{};
			std::chrono::steady_clock::time_point     refreshed{};
			bool                                      railFound = false;
			bool                                      truncated = false;   // more buildings than kMaxPeers were seen
			float                                     capacity  = -1.0f;   // items a minute; negative when unread
			std::vector<MassPeer>                     peers;
			std::vector<RailNetwork::FlowPart>        parts;   // each peer's part in the item sums, rebuilt each refresh
			std::unordered_map<std::string, ItemInfo> items;   // every item seen on its drones
			RailNetwork::FlowMeter                    meter;
		};

		std::vector<Watch> g_watches;

		double Seconds(std::chrono::steady_clock::time_point t)
		{
			return std::chrono::duration<double>(t.time_since_epoch()).count();
		}

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

		// One peer's part in its item's sums, from its crafting fragment: a
		// crafter making the item adds its output; a crafter taking it as an
		// ingredient adds its intake. Storage, stations and a crafter handing out
		// what it does not make (leftovers) are stock, with no rate.
		//
		// A source is only added once a drone has been seen carrying the item from
		// it: an empty drone on its way to a pickup may be on a stretch before the
		// pickup, which the item never passes.
		void RateOf(const MassPeer& peer, std::vector<RailNetwork::FlowPart>& parts)
		{
			if (peer.source && !peer.carried)
				return;

			const std::string& name = peer.buildingName.empty() ? kUnknownBuilding : peer.buildingName;
			RailMass::CrafterState crafter;
			const bool isCrafter = RailMass::ReadCrafter(peer.entity, crafter) && crafter.isCrafter;
			UAuItemRecipeData* recipe = isCrafter && Features::Crafters.On() ? crafter.recipe : nullptr;
			const int stalled = isCrafter ? crafter.stalled : -1;

			if (recipe)
			{
				UAuItemDataBase* made = recipe->GetItemDataBase();
				if (peer.source && made && Fields::UniqueNameOf(made) == peer.itemUnique)
				{
					float full = -1.0f;
					if (!RecipeRate(recipe, 0, crafter.speed, full))
						full = -1.0f;
					RailNetwork::FlowPart part{ peer.entity, peer.itemUnique, true, true, full };
					part.now  = full >= 0.0f ? (crafter.stalled ? 0.0f : full) : -1.0f;
					part.name = name;
					parts.push_back(std::move(part));
					return;
				}
				if (!peer.source)
				{
					std::vector<Input> inputs = InputsOf(recipe, crafter.speed);
					inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
						[&](const Input& in) { return in.item != peer.itemUnique; }), inputs.end());
					if (!inputs.empty())
					{
						AddTaker(peer.entity, name, inputs, stalled, parts);
						return;
					}
				}
			}

			// A crafter whose rate is unread stays a crafter, so its unknown rate
			// leaves the item's totals unknown rather than wrong.
			RailNetwork::FlowPart part{ peer.entity, peer.itemUnique, peer.source, isCrafter && !recipe };
			part.name = name;
			parts.push_back(std::move(part));
		}

		void Remember(Watch& w, UAuItemDataBase* item, const std::string& unique)
		{
			if (!item || w.items.count(unique))
				return;
			ItemInfo info;
			info.item.ObjectIndex        = item->Index;
			info.item.ObjectSerialNumber = 0;
			info.displayName = Fields::DisplayNameOf(item);
			w.items.emplace(unique, std::move(info));
		}

		void Merge(Watch& w, const RailMass::Observation& seen)
		{
			for (const RailMass::Peer& p : seen.peers)
			{
				if (!p.item)
					continue;
				const std::string unique = Fields::UniqueNameOf(p.item);
				const auto same = std::find_if(w.peers.begin(), w.peers.end(), [&](const MassPeer& m)
					{ return m.entity == p.entity && m.source == p.source && m.itemUnique == unique; });
				if (same != w.peers.end())
				{
					same->carried = same->carried || p.carried;
					continue;
				}
				if (w.peers.size() >= kMaxPeers)
				{
					w.truncated = true;
					continue;
				}

				Remember(w, p.item, unique);
				MassPeer peer;
				peer.entity     = p.entity;
				peer.source     = p.source;
				peer.carried    = p.carried;
				peer.itemUnique = unique;
				w.peers.push_back(std::move(peer));
			}
		}

		// One sample of the rail's lane: its ends, and the items moving on it for
		// the meter. False when the rail's lane is gone or the reader is off.
		bool Sample(Watch& w, std::chrono::steady_clock::time_point now)
		{
			w.sampled = now;
			RailMass::Observation seen;
			if (!RailMass::Observe(w.rail, seen) || !seen.railFound)
				return false;

			w.railFound = true;
			Merge(w, seen);

			std::vector<RailNetwork::FlowMeter::OnLane> lane;
			for (const RailMass::Carrying& c : seen.lane)
			{
				const std::string unique = Fields::UniqueNameOf(c.item);
				Remember(w, c.item, unique);
				lane.push_back({ c.drone, unique });
			}
			w.meter.Sample(Seconds(now), lane);
			return true;
		}

		// The rail's capacity, and each peer's name and part in the sums.
		void Refresh(Watch& w, std::chrono::steady_clock::time_point now)
		{
			w.refreshed = now;
			RailMass::ReadCapacity(w.rail, w.capacity);

			w.parts.clear();
			for (MassPeer& peer : w.peers)
			{
				if (peer.buildingName.empty())
					peer.buildingName = BuildingNameOf(peer.entity);
				RateOf(peer, w.parts);
			}
		}

		// The rail's watch, started when it is first aimed at. Null when the rail
		// has no Mass entity or its lane cannot be read.
		Watch* WatchOf(AActor* rail, std::chrono::steady_clock::time_point now)
		{
			const RailMass::Entity entity = RailMass::EntityOf(rail);
			if (!entity)
				return nullptr;

			auto found = std::find_if(g_watches.begin(), g_watches.end(), [&](const Watch& w) { return w.rail == entity; });
			if (found == g_watches.end())
			{
				if (g_watches.size() >= kMaxWatches)
					g_watches.erase(std::min_element(g_watches.begin(), g_watches.end(),
						[](const Watch& a, const Watch& b) { return a.aimed < b.aimed; }));
				Watch w;
				w.rail = entity;
				if (!Sample(w, now))
					return nullptr;
				Refresh(w, now);
				g_watches.push_back(std::move(w));
				found = g_watches.end() - 1;
			}
			found->aimed = now;
			return &*found;
		}

		// What the card shows, rebuilt when either way of looking refreshes or the
		// aim moves to another rail: every building's part, from both, and the
		// totals summed from them. `rail` is only compared.
		const AActor*                      g_partsRail = nullptr;
		std::vector<RailNetwork::FlowPart> g_parts;
		std::vector<RailNetwork::ItemFlow> g_flows;

		// An item's name and icon: from the layout, or else from the drones.
		void DescribeItem(const std::string& unique, const Watch* w, std::string& display, UAuItemDataBase*& data)
		{
			FWeakObjectPtr item{};
			const auto info = g_cache.items.find(unique);
			if (info != g_cache.items.end())
			{
				display = info->second.displayName;
				item    = info->second.item;
			}
			else if (w)
			{
				const auto seen = w->items.find(unique);
				if (seen != w->items.end())
				{
					display = seen->second.displayName;
					item    = seen->second.item;
				}
			}
			// A dead item just loses its icon.
			UObject* obj = item.Get();
			if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
				data = static_cast<UAuItemDataBase*>(obj);
		}
	}

	bool RailDetector::IsRail(AActor* actor)
	{
		if (!actor || !UKismetSystemLibrary::IsValid(actor))
			return false;

		return actor->IsA(ACrDronePathPointConnection::StaticClass());
	}

	void RailDetector::Tick()
	{
		if (g_watches.empty())
			return;
		if (!Features::ShowRailSources())
		{
			g_watches.clear();
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(), [&](Watch& w)
			{
				if (now - w.aimed >= kWatchFor)
					return true;
				return now - w.sampled >= kSampleEvery && !Sample(w, now);
			}), g_watches.end());
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

		// The rail's ends and traffic from the Mass data.
		Watch* watch = nullptr;
		bool massRefreshed = false;
		if (Features::ShowRailSources())
		{
			const size_t before = g_watches.size();
			watch = WatchOf(actor, now);
			massRefreshed = g_watches.size() != before;
			if (watch && !massRefreshed && now - watch->refreshed >= kRefresh)
			{
				Refresh(*watch, now);
				massRefreshed = true;
			}
			outInfo.fromMass = true;
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

		// Every building's part: the layout's first (its sources carry the queued
		// output count), then the drones', each building counted once. A filtered
		// rail only carries that item.
		if (refreshed || massRefreshed || g_partsRail != actor)
		{
			g_partsRail = actor;
			g_parts.clear();
			if (Features::ShowDroneRails() && g_cache.rail == actor)
				g_parts = g_cache.parts;
			if (watch)
				g_parts.insert(g_parts.end(), watch->parts.begin(), watch->parts.end());
			if (!filterUnique.empty())
				g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(),
					[&](const RailNetwork::FlowPart& p) { return p.item != filterUnique; }), g_parts.end());

			const float capacity = watch && Features::ShowRailCapacity() ? watch->capacity : -1.0f;
			g_flows = RailNetwork::SumFlows(g_parts, capacity);
		}

		// One row per item: the totals, with now measured by the watch. An item
		// seen crossing the rail with no source found still gets its row.
		const double t = Seconds(now);
		const bool measuring = watch && !watch->meter.Ready(t);
		std::unordered_set<std::string> shown;
		auto addRow = [&](const std::string& unique, float max, float min)
		{
			RailRateEntry entry;
			entry.perMinMax = max;
			entry.perMinMin = min;
			entry.perMinNow = watch ? watch->meter.PerMin(t, unique) : -1.0f;
			entry.measuring = measuring;
			DescribeItem(unique, watch, entry.displayName, entry.itemData);
			outInfo.rates.push_back(std::move(entry));
			shown.insert(unique);
		};
		for (const RailNetwork::ItemFlow& flow : g_flows)
		{
			outInfo.sourceCount += flow.sources;
			addRow(flow.item, flow.max, flow.min);
		}
		if (watch)
			for (const std::string& unique : watch->meter.Items(t))
				if (!shown.count(unique) && (filterUnique.empty() || unique == filterUnique))
					addRow(unique, -1.0f, -1.0f);
		outInfo.measuring = measuring;
		outInfo.truncated = g_cache.rail == actor && g_cache.truncated;

		// The From/To lines, from the same parts as the totals.
		if (watch)
			outInfo.peersTruncated = watch->truncated;
		for (const RailNetwork::EndRow& end : RailNetwork::GroupEnds(g_parts, shown))
		{
			RailPeerEntry entry;
			entry.source       = end.source;
			entry.buildingName = end.name.empty() ? kUnknownBuilding : end.name;
			entry.count        = end.count;
			entry.perMinFull   = end.full;
			entry.perMinNow    = end.now;
			DescribeItem(end.item, watch, entry.itemName, entry.itemData);
			(end.source ? outInfo.fromCount : outInfo.destCount) += end.count;
			outInfo.peers.push_back(std::move(entry));
		}

		if (outInfo.rates.empty() && outInfo.peers.empty() && outInfo.fromMass)
			outInfo.note = !watch ? "Rail data not available" : measuring ? "Measuring..." : "No cargo moving";

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
		g_watches.clear();
		g_partsRail = nullptr;
		g_parts.clear();
		g_flows.clear();
	}
}
