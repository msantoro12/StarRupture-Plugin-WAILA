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

		// Items per minute of one recipe at a crafting speed: output count /
		// BuildTime x speed x 60. countOverride (when above 0) replaces the
		// recipe's own output count.
		bool RecipeRate(UAuItemRecipeData* recipe, int32_t countOverride, float speed, float& outFull)
		{
			const int32_t count = countOverride > 0 ? countOverride : Fields::RecipeOutputCount(recipe);
			const float buildTime = Fields::ValueOr<float>(Fields::RecipeBuildTime, recipe);
			if (count <= 0 || buildTime <= 0.0f || !(speed > 0.0f) || !std::isfinite(speed))
				return false;

			outFull = static_cast<float>(count) / buildTime * speed * 60.0f;
			return true;
		}

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
			outItem = recipe->GetItemDataBase();
			if (!outItem)
				return false;

			const FCrCraftingSettings* settings = Fields::CraftingSettings.In(crafter);
			const float speed = frag ? Fields::ValueOr<float>(Fields::FragSpeed, frag)
			                         : Fields::ValueOr<float>(Fields::SettingsSpeed, settings);
			return RecipeRate(recipe, queued ? first.outputCount : 0, speed, outFull);
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

		// ---- From the game's Mass data (rail_mass.h) ---------------------------

		// More than this many distinct buildings stops being a glance.
		constexpr size_t kMaxPeers = 32;

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
			std::string      buildingName;   // empty until the building is loaded once
			float            perMinNow = -1.0f;
			float            perMinMax = -1.0f;
		};

		struct MassCache
		{
			const AActor*                         rail = nullptr;
			std::chrono::steady_clock::time_point stamp{};
			bool                                  railFound = false;
			int32_t                               drones    = 0;
			std::vector<MassPeer>                 peers;
		};

		MassCache g_massCache;

		// The name of every loaded building by its Mass entity.
		std::unordered_map<RailMass::Entity, std::string> LoadedBuildingNames(UWorld* world)
		{
			std::unordered_map<RailMass::Entity, std::string> names;
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDroneSupplySystemBuildingBase::StaticClass(), &all);
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				AActor* building = all[i];
				const RailMass::Entity entity = RailMass::EntityOf(building);
				if (!entity)
					continue;
				std::string name, desc;
				if (Fields::ReadBuildingTexts(Fields::BuildingDataOf(static_cast<ACrBuildingActorBase*>(building)), name, desc))
					names.emplace(entity, std::move(name));
			}
			return names;
		}

		// A crafter's rate for the item the rail moves: only when it makes that item.
		void RateOf(MassPeer& peer)
		{
			peer.perMinNow = peer.perMinMax = -1.0f;
			RailMass::CrafterState crafter;
			if (!Features::Crafters.On() || !RailMass::ReadCrafter(peer.entity, crafter) || !crafter.recipe)
				return;

			UAuItemDataBase* made = crafter.recipe->GetItemDataBase();
			float full = 0.0f;
			if (!made || Fields::UniqueNameOf(made) != peer.itemUnique
				|| !RecipeRate(crafter.recipe, 0, crafter.speed, full))
				return;
			peer.perMinMax = full;
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

		void RefreshMass(AActor* rail, UWorld* world, MassCache& cache)
		{
			RailMass::Observation seen;
			const RailMass::Entity entity = RailMass::EntityOf(rail);
			if (entity && RailMass::Observe(entity, seen))
			{
				cache.railFound = seen.railFound;
				cache.drones    = seen.drones;
				Merge(cache, seen);
			}

			std::unordered_map<RailMass::Entity, std::string> names;
			bool namesRead = false;
			for (MassPeer& peer : cache.peers)
			{
				if (peer.buildingName.empty())
				{
					if (!namesRead)
					{
						names = LoadedBuildingNames(world);
						namesRead = true;
					}
					const auto found = names.find(peer.entity);
					if (found != names.end())
						peer.buildingName = found->second;
				}
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

		if (Features::ShowRailSources())
		{
			if (g_massCache.rail != actor || now - g_massCache.stamp >= kRefresh)
			{
				UWorld* world = PluginSdkRuntime::GetWorld();
				if (!world)
					return true;

				if (g_massCache.rail != actor)
					g_massCache = MassCache{};
				g_massCache.rail  = actor;
				g_massCache.stamp = now;
				RefreshMass(actor, world, g_massCache);
			}

			outInfo.fromMass = true;
			for (const MassPeer& peer : g_massCache.peers)
			{
				// A filtered rail only carries that item.
				if (!filterUnique.empty() && peer.itemUnique != filterUnique)
					continue;

				RailPeerEntry entry;
				entry.source       = peer.source;
				entry.buildingName = peer.buildingName.empty() ? "Out of range" : peer.buildingName;
				entry.itemName     = peer.itemDisplay;
				entry.perMinNow    = peer.perMinNow;
				entry.perMinMax    = peer.perMinMax;

				// A dead item just loses its icon.
				UObject* obj = peer.item.Get();
				if (obj && UKismetSystemLibrary::IsValid(obj) && obj->IsA(UAuItemDataBase::StaticClass()))
					entry.itemData = static_cast<UAuItemDataBase*>(obj);

				++(peer.source ? outInfo.sourceCount : outInfo.destCount);
				outInfo.peers.push_back(std::move(entry));
			}
			// Sources first.
			std::stable_partition(outInfo.peers.begin(), outInfo.peers.end(),
				[](const RailPeerEntry& e) { return e.source; });

			if (outInfo.peers.empty())
				outInfo.note = g_massCache.railFound ? "No cargo moving" : "Rail data not available";
			return true;
		}

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
		g_massCache = MassCache{};
	}
}
