#include "rail_detector.h"
#include "plugin_helpers.h"
#include "waila_functions.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"

#include <algorithm>
#include <array>
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
		// Bounds for the network scan. Both stop a runaway fill and set
		// RailInfo::truncated when they bite.
		constexpr size_t kMaxRails   = 1024;
		constexpr size_t kMaxSources = 256;

		// INFERRED, as is everything about which buildings a rail serves: the SDK
		// has no explicit building-to-rail link. ConnectedStartActor and
		// ConnectedEndActor were null on every rail probed in a loaded save, so the
		// topology comes from the rail splines instead. Two rails are neighbours when
		// an end of one lies within kJoinTolerance of an end of the other. A crafter is
		// a source when a rail end lies inside its bounds grown by kCrafterMargin, and
		// the fill does not continue through that end. A building that only consumes
		// from the rail cannot be told apart from one that feeds it.
		//
		// Only actors the game has spawned are visible to GetAllActorsOfClass, and
		// player buildings are streamed in around the camera (INFERRED), so a long
		// rail can be cut short without `truncated` being set.
		constexpr double kJoinTolerance = 100.0;   // unreal units, 100 = 1 m
		constexpr double kCrafterMargin = 200.0;   // unreal units, 200 = 2 m

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

		// A crafter and its bounds, fetched once per scan. The pointer is only used
		// inside that scan.
		struct CrafterBox
		{
			ACrCrafter* crafter;
			FVector     origin;
			FVector     extent;
		};

		std::vector<CrafterBox> GatherCrafters(UWorld* world)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrCrafter::StaticClass(), &all);

			std::vector<CrafterBox> boxes;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* crafter = static_cast<ACrCrafter*>(all[i]);
				if (!crafter || !UKismetSystemLibrary::IsValid(crafter))
					continue;

				CrafterBox box{ crafter, {}, {} };
				crafter->GetActorBounds(false, &box.origin, &box.extent, false);
				boxes.push_back(box);
			}
			return boxes;
		}

		// World positions of a rail's first and last spline point. False when the
		// spline is missing.
		bool RailEnds(ACrDronePathPointConnection* rail, std::array<FVector, 2>& out)
		{
			USplineComponent* spline = rail->Spline;
			if (!spline || !UKismetSystemLibrary::IsValid(spline))
				return false;

			const int32_t points = spline->GetNumberOfSplinePoints();
			if (points < 2)
				return false;

			out[0] = spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
			out[1] = spline->GetLocationAtSplinePoint(points - 1, ESplineCoordinateSpace::World);
			return true;
		}

		bool InsideBox(const FVector& p, const CrafterBox& box)
		{
			return std::abs(p.X - box.origin.X) <= box.extent.X + kCrafterMargin
			    && std::abs(p.Y - box.origin.Y) <= box.extent.Y + kCrafterMargin
			    && std::abs(p.Z - box.origin.Z) <= box.extent.Z + kCrafterMargin;
		}

		// Rail ends are bucketed in a grid of kJoinTolerance cells, so finding the ends
		// near a point only checks the 27 cells around it.
		struct Cell
		{
			int64_t x, y, z;
			bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
		};

		struct CellHash
		{
			size_t operator()(const Cell& c) const
			{
				return static_cast<size_t>(c.x * 73856093LL ^ c.y * 19349663LL ^ c.z * 83492791LL);
			}
		};

		Cell CellOf(const FVector& p)
		{
			return { static_cast<int64_t>(std::floor(p.X / kJoinTolerance)),
			         static_cast<int64_t>(std::floor(p.Y / kJoinTolerance)),
			         static_cast<int64_t>(std::floor(p.Z / kJoinTolerance)) };
		}

		struct RailEnd
		{
			size_t  rail;   // index into the scan's rail list
			FVector at;
		};

		// Flood fill over the rail segments, from the looked-at one outwards. Rails
		// meet where their spline ends do, so two rails are neighbours when an end of
		// one lies within kJoinTolerance of an end of the other.
		// An end that no other rail meets and that lies inside a crafter's bounds
		// makes that crafter a source. Nothing continues from such an end, so a
		// factory's input and output rails do not merge into one network. Rails only
		// join end to end: the sockets of a junction, roundabout or vertical connector
		// sit metres apart, so the fill does not cross those buildings.
		// Fills outDirect with the crafters and outRails with the rails visited.
		void CollectNetwork(UWorld* world, ACrDronePathPointConnection* start,
		                    const std::vector<CrafterBox>& crafters,
		                    std::unordered_set<const AActor*>& outDirect,
		                    std::unordered_set<const AActor*>& outRails, bool& outTruncated)
		{
			TArray<AActor*> all{};
			UGameplayStatics::GetAllActorsOfClass(world, ACrDronePathPointConnection::StaticClass(), &all);

			std::vector<ACrDronePathPointConnection*>                rails;
			std::vector<std::array<FVector, 2>>                      ends;
			std::unordered_map<Cell, std::vector<RailEnd>, CellHash> grid;
			for (int32_t i = 0; i < all.Num(); ++i)
			{
				auto* rail = static_cast<ACrDronePathPointConnection*>(all[i]);
				std::array<FVector, 2> e;
				if (!rail || !UKismetSystemLibrary::IsValid(rail) || !RailEnds(rail, e))
					continue;

				grid[CellOf(e[0])].push_back({ rails.size(), e[0] });
				grid[CellOf(e[1])].push_back({ rails.size(), e[1] });
				rails.push_back(rail);
				ends.push_back(e);
			}

			outRails.insert(start);

			const auto startIt = std::find(rails.begin(), rails.end(), start);
			if (startIt == rails.end())
				return;

			std::vector<bool>   seen(rails.size(), false);
			std::vector<size_t> queue{ static_cast<size_t>(startIt - rails.begin()) };
			seen[queue[0]] = true;

			const double toleranceSq = kJoinTolerance * kJoinTolerance;

			for (size_t head = 0; head < queue.size(); ++head)
			{
				const size_t current = queue[head];

				for (const FVector& at : ends[current])
				{
					// The other rails whose ends meet this one.
					std::vector<size_t> meets;
					const Cell c = CellOf(at);
					for (int64_t dx = -1; dx <= 1; ++dx)
					for (int64_t dy = -1; dy <= 1; ++dy)
					for (int64_t dz = -1; dz <= 1; ++dz)
					{
						const auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
						if (it == grid.end())
							continue;

						for (const RailEnd& other : it->second)
						{
							if (other.rail == current)
								continue;

							const double ex = at.X - other.at.X, ey = at.Y - other.at.Y, ez = at.Z - other.at.Z;
							if (ex * ex + ey * ey + ez * ez <= toleranceSq)
								meets.push_back(other.rail);
						}
					}

					// An end nothing else meets is where the rail reaches a building.
					// It serves the one crafter whose bounds hold it, and the fill stops.
					if (meets.empty())
					{
						const CrafterBox* nearest = nullptr;
						double            nearestSq = 0.0;
						for (const CrafterBox& box : crafters)
						{
							if (!InsideBox(at, box))
								continue;

							const double bx = at.X - box.origin.X, by = at.Y - box.origin.Y, bz = at.Z - box.origin.Z;
							const double sq = bx * bx + by * by + bz * bz;
							if (!nearest || sq < nearestSq)
							{
								nearest   = &box;
								nearestSq = sq;
							}
						}
						if (nearest)
							outDirect.insert(nearest->crafter);
						continue;
					}

					for (size_t next : meets)
					{
						if (seen[next])
							continue;

						if (queue.size() >= kMaxRails)
						{
							outTruncated = true;
							return;
						}
						seen[next] = true;
						queue.push_back(next);
						outRails.insert(rails[next]);
					}
				}
			}
		}

		// Crafters on the network, summed per output item. Stations, storage and
		// receivers hold stock but have no production rate, so they are not sources.
		void CollectSources(const std::vector<CrafterBox>& crafters,
		                    const std::unordered_set<const AActor*>& direct, Cache& out)
		{
			std::unordered_map<std::string, size_t> index;

			for (const CrafterBox& box : crafters)
			{
				ACrCrafter* crafter = box.crafter;
				if (direct.count(crafter) == 0)
					continue;

				UAuItemDataBase* item = nullptr;
				float full = 0.0f;
				const bool readable = CrafterFullRate(crafter, item, full);

				// No recipe means no item to attribute anything to.
				if (!item)
					continue;

				const std::string key = item->UniqueItemName.ToString();

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

			const std::vector<CrafterBox> crafters = GatherCrafters(world);
			std::unordered_set<const AActor*> direct;
			CollectNetwork(world, rail, crafters, direct, g_cache.members, g_cache.truncated);
			CollectSources(crafters, direct, g_cache);

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
