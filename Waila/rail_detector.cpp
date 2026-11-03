#include "rail_detector.h"
#include "rail_network.h"
#include "rail_mass.h"
#include "storage_detector.h"
#include "crafter_detector.h"
#include "plugin_helpers.h"
#include "waila_functions.h"
#include "waila_fields.h"
// WAILA-RAIL-DIAG BEGIN
#include "rail_diag.h"
// WAILA-RAIL-DIAG END
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
		// Bounds for the network walk. Both stop a runaway walk and set
		// RailInfo::truncated when they bite.
		constexpr size_t kMaxRails   = 1024;
		constexpr size_t kMaxSources = 256;

		constexpr std::chrono::milliseconds kRefresh{ 1000 };

		// Everything the walk found for one rail, kept between refreshes: each
		// machine's part in the flow of an item (rail_network.h, SumFlows), the
		// totals, and the items' names. Item data is held as a weak pointer and
		// resolved on use. `rail` is only ever compared, never dereferenced: a
		// recycled address could at worst show the previous rail's numbers until
		// the next refresh.
		//
		// The walk is the game's own rail network from its Mass data, loaded or
		// not (FindAttached, rail_mass.h). When the Mass reader is off, or its walk
		// finds no maker, the loaded rail layout stands in: only actors the game has spawned are visible
		// to GetAllActorsOfClass, and player buildings are streamed in around the
		// camera (INFERRED), so that one can cut a long rail short.
		struct ItemInfo
		{
			FWeakObjectPtr item;
			std::string    displayName;
		};

		struct Cache
		{
			const AActor*                             rail = nullptr;
			std::chrono::steady_clock::time_point     stamp{};
			bool                                      fromMass  = false;
			bool                                      found     = false;   // the Mass walk could start from the rail
			bool                                      truncated = false;
			float                                     capacity  = -1.0f;   // items a minute; negative when unread
			std::vector<RailNetwork::FlowPart>        parts;
			std::vector<RailNetwork::ItemFlow>        flows;
			std::unordered_map<std::string, ItemInfo> items;

			// What the storage the walk passed through holds, per item, and the
			// items in the order first found.
			struct Stock
			{
				int64_t count    = 0;
				int32_t storages = 0;
			};
			std::unordered_map<std::string, Stock> stock;
			std::vector<std::string>               stockOrder;
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

		// The layout names a crafter by its address, only ever compared.
		uint64_t BuildingKey(ACrCrafter* crafter)
		{
			return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(crafter));
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

		void Remember(Cache& out, UAuItemDataBase* item, const std::string& unique)
		{
			if (!item || out.items.count(unique))
				return;
			ItemInfo info;
			info.item.ObjectIndex        = item->Index;
			info.item.ObjectSerialNumber = 0;
			info.displayName = Fields::DisplayNameOf(item);
			out.items.emplace(unique, std::move(info));
		}

		// The crafters the rail layout finds: the ones feeding the stretch, at
		// their output with every one running, and the ones taking items off the
		// line ahead of it. A crafter with no recipe has no item to count. Their
		// state is in the Mass data, which this fallback runs without, so no now
		// figure.
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
				Remember(out, item, unique);
				RailNetwork::FlowPart part{ BuildingKey(crafter), unique, true, readable ? full : -1.0f };
				part.name = NameOf(crafter);
				out.parts.push_back(std::move(part));
			}

			for (size_t consumer : consumers.crafters)
			{
				ACrCrafter* crafter = crafters[consumer];
				CrafterWork work;
				if (!crafter || !CrafterWorkOf(crafter, work))
					continue;
				for (const Input& in : InputsOf(work.recipe, work.speed))
				{
					RailNetwork::FlowPart taker{ BuildingKey(crafter), in.item, false, in.perMin };
					taker.name = NameOf(crafter);
					out.parts.push_back(std::move(taker));
				}
			}
		}

		// WAILA-RAIL-DIAG BEGIN
		uint64_t g_diagLastKey = 0;   // the rail last dumped: its entity, or for a rail without one its address

		void DiagEmit(const char* line)
		{
			LOG_INFO("%s", line);
		}

		// Starts a dump when the "Rail Diagnostic Dump" setting is on and the aimed
		// rail is not the one last dumped. Game thread only (the tick).
		bool BeginDiag(AActor* actor, bool fromMass, const std::string& filterUnique)
		{
			if (!RailDiag::g_enabled)
				return false;
			const RailMass::Entity entity = fromMass ? RailMass::EntityOf(actor) : 0;
			const uint64_t key = entity ? entity : (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(actor)) | (1ull << 63));
			if (key == g_diagLastKey)
				return false;
			g_diagLastKey = key;
			RailDiag::g_emit = &DiagEmit;
			RailDiag::Begin();
			RAIL_DIAG_LINE("rail entity %s (index/serial), actor class %s, filter \"%s\"", RailDiag::Id(entity).c_str(),
				actor && actor->Class ? actor->Class->GetName().c_str() : "?", filterUnique.c_str());
			RAIL_DIAG_LINE("features: mass reader %d, sources %d, crafters %d, capacity %d, loaded-layout fallback %d -> reading from %s",
				RailMass::Available(), Features::ShowRailSources(), Features::ShowCrafters(), Features::ShowRailCapacity(),
				Features::ShowDroneRails(), fromMass ? "the Mass data" : "the loaded layout only");
			return true;
		}
		// WAILA-RAIL-DIAG END

		// ---- From the game's Mass data (rail_mass.h) ---------------------------

		// The building's name from its Mass entity's building parameters, which
		// every entity keeps whether or not its actor is loaded.
		std::string BuildingNameOf(RailMass::Entity entity)
		{
			SDK::UCrBuildingData* data = nullptr;
			std::string name, desc;
			if (RailMass::ReadBuilding(entity, data) && Fields::ReadBuildingTexts(data, name, desc) && !name.empty())
				return name;
			return kUnknownBuilding;
		}

		// The machines attached to the rail's network (FindAttached) and each
		// one's part in the sums: an upstream crafter makes its recipe's item at
		// its full rate (0 now while stalled); a downstream crafter takes each
		// ingredient one of them makes, now only while it has a request open
		// for it. A filtered rail only carries that item. Storage is neither: it
		// is walked through, and its stock is counted on its own. False when the
		// rail's ends cannot be read.
		bool CollectAttached(RailMass::Entity rail, const std::string& filterUnique, Cache& out)
		{
			RailMass::Entity start = 0, end = 0;
			bool twoWay = false;
			if (!RailMass::ReadEnds(rail, start, end, twoWay))
			{
				// WAILA-RAIL-DIAG BEGIN
				RAIL_DIAG_LINE("rail %s: ReadEnds FAILED (start=%s end=%s, see the lines above): the card reads \"Rail data not available\"",
					RailDiag::Id(rail).c_str(), RailDiag::Id(start).c_str(), RailDiag::Id(end).c_str());
				// WAILA-RAIL-DIAG END
				return false;
			}
			std::vector<uint64_t> from{ start };
			if (twoWay)
				from.push_back(end);
			// WAILA-RAIL-DIAG BEGIN
			RAIL_DIAG_LINE("rail %s joins start node %s and end node %s; %s", RailDiag::Id(rail).c_str(),
				RailDiag::Id(start).c_str(), RailDiag::Id(end).c_str(),
				twoWay ? "two-way: both nodes seed the walk" : "one-way: only the start node seeds the walk (the end node is read once below, for the dump only)");
			if (RailDiag::Active() && !twoWay)
			{
				RailNetwork::NodeView endView;
				RailMass::ReadNode(end, endView);
			}
			// WAILA-RAIL-DIAG END

			const RailNetwork::Attached attached = RailNetwork::FindAttached(from,
				[](uint64_t node, RailNetwork::NodeView& view) { return RailMass::ReadNode(node, view); },
				kMaxRails, kMaxSources, rail);
			out.truncated = attached.truncated;
			// WAILA-RAIL-DIAG BEGIN
			RAIL_DIAG_LINE("walk result: %zu upstream machine(s), %zu downstream machine(s), %zu upstream storage, %zu downstream storage, truncated=%d",
				attached.upstream.size(), attached.downstream.size(), attached.upstreamStorage.size(),
				attached.downstreamStorage.size(), attached.truncated);
			// WAILA-RAIL-DIAG END

			std::unordered_set<std::string> made;
			for (const RailMass::Entity machine : attached.upstream)
			{
				RailMass::CrafterState crafter;
				if (!RailMass::ReadCrafter(machine, crafter) || !crafter.isCrafter || !crafter.recipe)
				{
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  upstream machine %s \"%s\" SKIPPED: %s", RailDiag::Id(machine).c_str(), BuildingNameOf(machine).c_str(),
						!crafter.isCrafter ? "no crafting data (not a crafter, or the crafter read is off/faulted; see the line above)"
						                   : "crafter without a usable recipe");
					// WAILA-RAIL-DIAG END
					continue;
				}
				UAuItemDataBase* item = crafter.recipe->GetItemDataBase();
				if (!item)
				{
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  upstream machine %s \"%s\" SKIPPED: its recipe has no output item", RailDiag::Id(machine).c_str(),
						BuildingNameOf(machine).c_str());
					// WAILA-RAIL-DIAG END
					continue;
				}
				const std::string unique = Fields::UniqueNameOf(item);
				if (!filterUnique.empty() && unique != filterUnique)
				{
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  upstream machine %s \"%s\" SKIPPED: makes %s but the rail's filter is %s", RailDiag::Id(machine).c_str(),
						BuildingNameOf(machine).c_str(), unique.c_str(), filterUnique.c_str());
					// WAILA-RAIL-DIAG END
					continue;
				}

				Remember(out, item, unique);
				made.insert(unique);
				float full = -1.0f;
				if (!RecipeRate(crafter.recipe, 0, crafter.speed, full))
					full = -1.0f;
				RailNetwork::FlowPart part{ machine, unique, true, full };
				part.now  = full >= 0.0f ? (crafter.stalled ? 0.0f : full) : -1.0f;
				part.name = BuildingNameOf(machine);
				// WAILA-RAIL-DIAG BEGIN
				RAIL_DIAG_LINE("  upstream machine %s \"%s\": FROM part, makes %s (%s) full=%.2f/min now=%.2f/min speed=%.2f stalled=%d "
					"[direction: reached walking against the flow, so its output can reach the rail]",
					RailDiag::Id(machine).c_str(), part.name.c_str(), unique.c_str(), Fields::DisplayNameOf(item).c_str(),
					part.full, part.now, crafter.speed, crafter.stalled);
				// WAILA-RAIL-DIAG END
				out.parts.push_back(std::move(part));
			}

			std::vector<UAuItemDataBase*> wanted;
			for (const RailMass::Entity machine : attached.downstream)
			{
				RailMass::CrafterState crafter;
				if (!RailMass::ReadCrafter(machine, crafter) || !crafter.isCrafter || !crafter.recipe)
				{
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  downstream machine %s \"%s\" SKIPPED: %s", RailDiag::Id(machine).c_str(), BuildingNameOf(machine).c_str(),
						!crafter.isCrafter ? "no crafting data (not a crafter, or the crafter read is off/faulted; see the line above)"
						                   : "crafter without a usable recipe");
					// WAILA-RAIL-DIAG END
					continue;
				}
				std::vector<Input> inputs = InputsOf(crafter.recipe, crafter.speed);
				// WAILA-RAIL-DIAG BEGIN
				std::string diagInputs;
				if (RailDiag::Active())
					for (const Input& in : inputs)
						diagInputs += in.item + (made.count(in.item) ? "(made upstream) " : "(not made upstream) ");
				// WAILA-RAIL-DIAG END
				inputs.erase(std::remove_if(inputs.begin(), inputs.end(),
					[&](const Input& in) { return !made.count(in.item); }), inputs.end());
				if (inputs.empty())
				{
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  downstream machine %s \"%s\" SKIPPED: none of its inputs is made upstream: %s", RailDiag::Id(machine).c_str(),
						BuildingNameOf(machine).c_str(), diagInputs.empty() ? "(recipe has no readable inputs)" : diagInputs.c_str());
					// WAILA-RAIL-DIAG END
					continue;
				}

				const bool requestsRead = RailMass::ReadWanted(machine, wanted);
				const std::string name = BuildingNameOf(machine);
				for (const Input& in : inputs)
				{
					const bool requesting = std::any_of(wanted.begin(), wanted.end(),
						[&](UAuItemDataBase* w) { return Fields::UniqueNameOf(w) == in.item; });
					RailNetwork::FlowPart taker{ machine, in.item, false, in.perMin };
					taker.now  = !requestsRead ? -1.0f : requesting ? in.perMin : 0.0f;
					taker.name = name;
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  downstream machine %s \"%s\": TO part, takes %s full=%.2f/min now=%.2f/min (requests read=%d, requesting=%d) "
						"[direction: reached walking with the flow from the start node or an upstream machine, and it takes an item made upstream]",
						RailDiag::Id(machine).c_str(), name.c_str(), in.item.c_str(), taker.full, taker.now, requestsRead, requesting);
					// WAILA-RAIL-DIAG END
					out.parts.push_back(std::move(taker));
				}
			}

			// The stock of every storage the walks passed through, counted once:
			// all of what the storage upstream holds, which can leave on the
			// rail, and of the storage downstream the items the rail carries.
			std::unordered_set<uint64_t> counted;
			std::vector<StoredItemEntry> held;
			auto count = [&](const std::vector<uint64_t>& storages, bool upstream)
			{
				for (const uint64_t storage : storages)
				{
					if (!counted.insert(storage).second || !RailMass::ReadStored(storage, held))
					{
						// WAILA-RAIL-DIAG BEGIN
						RAIL_DIAG_LINE("  %s storage %s SKIPPED: already counted, or its stock could not be read (stock read off or faulted, or see the line above)",
							upstream ? "upstream" : "downstream", RailDiag::Id(storage).c_str());
						// WAILA-RAIL-DIAG END
						continue;
					}
					// WAILA-RAIL-DIAG BEGIN
					RAIL_DIAG_LINE("  %s storage %s \"%s\": stock read, %zu item(s) [direction: walked through %s the rail's network; %s]",
						upstream ? "upstream" : "downstream", RailDiag::Id(storage).c_str(), BuildingNameOf(storage).c_str(), held.size(),
						upstream ? "against the flow" : "with the flow",
						upstream ? "all its items can leave on the rail" : "only items the rail carries count");
					// WAILA-RAIL-DIAG END
					for (const StoredItemEntry& entry : held)
					{
						if (entry.uniqueName.empty() || (!filterUnique.empty() && entry.uniqueName != filterUnique))
						{
							// WAILA-RAIL-DIAG BEGIN
							RAIL_DIAG_LINE("    stock %s x%lld skipped: %s", entry.uniqueName.c_str(), static_cast<long long>(entry.count),
								entry.uniqueName.empty() ? "no item name" : "not the rail's filter item");
							// WAILA-RAIL-DIAG END
							continue;
						}
						if (!upstream && !made.count(entry.uniqueName) && !out.stock.count(entry.uniqueName))
						{
							// WAILA-RAIL-DIAG BEGIN
							RAIL_DIAG_LINE("    stock %s x%lld skipped: downstream storage, and nothing upstream makes this item",
								entry.uniqueName.c_str(), static_cast<long long>(entry.count));
							// WAILA-RAIL-DIAG END
							continue;
						}
						// WAILA-RAIL-DIAG BEGIN
						RAIL_DIAG_LINE("    stock %s x%lld counted", entry.uniqueName.c_str(), static_cast<long long>(entry.count));
						// WAILA-RAIL-DIAG END
						Remember(out, entry.itemData, entry.uniqueName);
						const auto [at, added] = out.stock.try_emplace(entry.uniqueName);
						if (added)
							out.stockOrder.push_back(entry.uniqueName);
						at->second.count += entry.count;
						++at->second.storages;
					}
				}
			};
			count(attached.upstreamStorage, true);
			count(attached.downstreamStorage, false);
			return true;
		}

		// An item's name and icon.
		void DescribeItem(const std::string& unique, std::string& display, UAuItemDataBase*& data)
		{
			const auto info = g_cache.items.find(unique);
			if (info == g_cache.items.end())
				return;
			display = info->second.displayName;
			// A dead item just loses its icon.
			UObject* obj = info->second.item.Get();
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
		const bool fromMass = Features::ShowRailSources() && Features::ShowCrafters();
		// WAILA-RAIL-DIAG BEGIN
		const bool diagDump = BeginDiag(actor, fromMass, filterUnique);
		// WAILA-RAIL-DIAG END
		if (g_cache.rail != actor || g_cache.fromMass != fromMass || now - g_cache.stamp >= kRefresh
			// WAILA-RAIL-DIAG BEGIN
			|| diagDump
			// WAILA-RAIL-DIAG END
			)
		{
			g_cache = Cache{};
			g_cache.rail     = actor;
			g_cache.stamp    = now;
			g_cache.fromMass = fromMass;

			if (fromMass)
			{
				const RailMass::Entity entity = RailMass::EntityOf(actor);
				g_cache.found = entity && CollectAttached(entity, filterUnique, g_cache);
				if (g_cache.found && Features::ShowRailCapacity())
					RailMass::ReadCapacity(entity, g_cache.capacity);
				// WAILA-RAIL-DIAG BEGIN
				else
					RAIL_DIAG_LINE("capacity: not read (rail found=%d, capacity feature %d)", g_cache.found, Features::ShowRailCapacity());
				// WAILA-RAIL-DIAG END
			}
			// WAILA-RAIL-DIAG BEGIN
			RAIL_DIAG_LINE("Mass walk done: found=%d, %zu part(s), stock items %zu, truncated=%d",
				g_cache.found, g_cache.parts.size(), g_cache.stockOrder.size(), g_cache.truncated);
			RAIL_DIAG_LINE("loaded-layout fallback %s", (!fromMass || g_cache.parts.empty()) && Features::ShowDroneRails()
				? "RUNS (no Mass parts, or Mass off; it adds crafters the loaded actors show)" : "does not run");
			// WAILA-RAIL-DIAG END

			if ((!fromMass || g_cache.parts.empty()) && Features::ShowDroneRails())
			{
				UWorld* world = PluginSdkRuntime::GetWorld();
				RailNetwork::Scene scene;
				const std::vector<const AActor*> rails    = world ? GatherRails(world, scene) : std::vector<const AActor*>{};
				const std::vector<ACrCrafter*>   crafters = world ? GatherBuildings(world, scene) : std::vector<ACrCrafter*>{};
				if (world)
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
				if (!filterUnique.empty())
					g_cache.parts.erase(std::remove_if(g_cache.parts.begin(), g_cache.parts.end(),
						[&](const RailNetwork::FlowPart& p) { return p.item != filterUnique; }), g_cache.parts.end());
			}
			g_cache.flows = RailNetwork::SumFlows(g_cache.parts, g_cache.capacity);
			// WAILA-RAIL-DIAG BEGIN
			if (RailDiag::Active())
			{
				RAIL_DIAG_LINE("flows summed from %zu part(s), capacity %.2f/min (negative = unread):", g_cache.parts.size(), g_cache.capacity);
				for (const RailNetwork::FlowPart& part : g_cache.parts)
					RAIL_DIAG_LINE("  part %s %s \"%s\" item %s full=%.2f now=%.2f", part.source ? "FROM" : "TO  ", RailDiag::Id(part.building).c_str(),
						part.name.c_str(), part.item.c_str(), part.full, part.now);
				for (const RailNetwork::ItemFlow& flow : g_cache.flows)
					RAIL_DIAG_LINE("  flow %s: %d source(s), %d consumer(s), output=%.2f -> min=%.2f now=%.2f max=%.2f", flow.item.c_str(),
						flow.sources, flow.consumers, flow.output, flow.min, flow.now, flow.max);
				if (g_cache.flows.empty())
					RAIL_DIAG_LINE("  no flows: SumFlows drops an item no attached machine makes, so a rail with takers but no maker shows no item row");
				for (const std::string& item : g_cache.stockOrder)
					RAIL_DIAG_LINE("  stock %s: %lld in %d storage(s)", item.c_str(), static_cast<long long>(g_cache.stock[item].count),
						g_cache.stock[item].storages);
			}
			// WAILA-RAIL-DIAG END
		}

		// One row per item made, each with its From/To lines (from the same
		// parts) and its stock, then the items only the storage upstream holds.
		std::unordered_set<std::string> made;
		for (const RailNetwork::ItemFlow& flow : g_cache.flows)
			made.insert(flow.item);
		const std::vector<RailNetwork::EndRow> ends = fromMass ? RailNetwork::GroupEnds(g_cache.parts, made)
		                                                       : std::vector<RailNetwork::EndRow>{};
		auto addStock = [](const std::string& item, RailRateEntry& entry)
		{
			const auto held = g_cache.stock.find(item);
			if (held == g_cache.stock.end())
				return;
			entry.stored   = held->second.count;
			entry.storages = held->second.storages;
		};
		for (const RailNetwork::ItemFlow& flow : g_cache.flows)
		{
			RailRateEntry entry;
			entry.perMinMax = flow.max;
			entry.perMinMin = flow.min;
			entry.perMinNow = flow.now;
			DescribeItem(flow.item, entry.displayName, entry.itemData);
			for (const RailNetwork::EndRow& end : ends)
			{
				if (end.item != flow.item)
					continue;
				RailPeerEntry peer;
				peer.source       = end.source;
				peer.buildingName = end.name.empty() ? kUnknownBuilding : end.name;
				peer.count        = end.count;
				peer.perMinFull   = end.full;
				peer.perMinNow    = end.now;
				peer.itemData     = entry.itemData;
				(end.source ? outInfo.fromCount : outInfo.destCount) += end.count;
				entry.peers.push_back(std::move(peer));
			}
			addStock(flow.item, entry);
			outInfo.rates.push_back(std::move(entry));
			outInfo.sourceCount += flow.sources;
		}
		for (const std::string& item : g_cache.stockOrder)
		{
			if (made.count(item))
				continue;
			RailRateEntry entry;
			entry.made = false;
			DescribeItem(item, entry.displayName, entry.itemData);
			addStock(item, entry);
			outInfo.rates.push_back(std::move(entry));
		}
		outInfo.truncated = g_cache.truncated;
		outInfo.fromMass  = fromMass;

		if (outInfo.rates.empty() && fromMass)
			outInfo.note = g_cache.found ? "No machines attached" : "Rail data not available";

		// WAILA-RAIL-DIAG BEGIN
		if (RailDiag::Active())
		{
			RAIL_DIAG_TAIL("handed to the card: fromCount=%d destCount=%d sourceCount=%d truncated=%d fromMass=%d filter=\"%s\" rows=%zu note=\"%s\"",
				outInfo.fromCount, outInfo.destCount, outInfo.sourceCount, outInfo.truncated, outInfo.fromMass, outInfo.filterName.c_str(),
				outInfo.rates.size(), outInfo.note.c_str());
			for (const RailRateEntry& r : outInfo.rates)
			{
				RAIL_DIAG_TAIL("  row \"%s\" made=%d min=%.2f now=%.2f max=%.2f stored=%lld in %d storage(s), %zu From/To row(s)",
					r.displayName.c_str(), r.made, r.perMinMin, r.perMinNow, r.perMinMax, static_cast<long long>(r.stored), r.storages,
					r.peers.size());
				for (const RailPeerEntry& p : r.peers)
					RAIL_DIAG_TAIL("    %s %dx \"%s\" full=%.2f now=%.2f", p.source ? "From" : "To", p.count, p.buildingName.c_str(),
						p.perMinFull, p.perMinNow);
			}
			if (outInfo.rates.empty())
				RAIL_DIAG_TAIL("note chosen at the end of RailDetector::GetRailInfo: no rows, fromMass=%d, Mass walk found the rail=%d -> %s "
					"(rows come only from items an attached machine makes, plus stock; header counts come from the From/To rows)",
					fromMass, g_cache.found, !fromMass ? "no note here, Card::FromRail then shows \"No sources found\""
					: g_cache.found ? "\"No machines attached\"" : "\"Rail data not available\"");
			RailDiag::Flush();
		}
		// WAILA-RAIL-DIAG END

		return true;
	}

	void RailDetector::Reset()
	{
		g_cache = Cache{};
	}
}
