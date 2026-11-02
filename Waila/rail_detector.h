#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SDK { class AActor; class UAuItemDataBase; class UCrBuildingData; }

namespace Waila
{
	// One item the rail carries. Rates are items per minute; a negative value
	// means it could not be read, and the card shows "?" for it.
	struct RailRateEntry
	{
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           displayName;
		float                 perMinMax = -1.0f;   // the sources' output, capped by what the rail carries
		float                 perMinMin = -1.0f;   // what is left if every machine ahead takes its full share
		float                 perMinNow = -1.0f;   // measured: what crossed the rail over the last minute
		bool                  measuring = false;   // the measurement has not run long enough yet
	};

	// The buildings of one kind at an end of the rail's traffic: where an item
	// comes from or goes to, one row for all the buildings of a name making or
	// taking that item. Rates are items per minute, summed over those buildings,
	// negative when not known or not a crafter.
	struct RailPeerEntry
	{
		bool                  source = false;   // false: the item goes there
		std::string           buildingName;
		int32_t               count = 1;   // buildings merged into this row
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           itemName;
		float                 perMinFull = -1.0f;   // running full out
		float                 perMinNow  = -1.0f;   // right now: 0 while stalled
	};

	struct RailInfo
	{
		std::string buildingName;
		std::string buildingDesc;
		SDK::UCrBuildingData* buildingData = nullptr;

		// What the rail is set to carry. Empty when it carries anything.
		std::string filterName;

		int32_t sourceCount = 0;
		bool    truncated   = false;   // the network or the source list hit a cap
		bool    measuring   = false;   // the rail has not been watched long enough for a now figure
		std::vector<RailRateEntry> rates;

		// The buildings the rail's items come from and go to, loaded or not, shown
		// under `rates`: the ones found in the rail layout and from the game's own
		// Mass data (rail_mass.h), which the rates are summed from.
		bool                       fromMass = false;
		int32_t                    fromCount = 0;
		int32_t                    destCount = 0;
		bool                       peersTruncated = false;   // more buildings than were kept
		std::vector<RailPeerEntry> peers;     // sources first
		std::string                note;      // stands in when there is nothing to list

		bool IsValid() const { return !buildingName.empty(); }
	};

	class RailDetector
	{
	public:
		static bool IsRail(SDK::AActor* actor);

		// Game thread only. The network scan is cached per rail and refreshed about
		// once a second, so calling this every tick is cheap. Aiming at a rail
		// starts watching it (see Tick).
		static bool GetRailInfo(SDK::AActor* actor, RailInfo& out);

		// Game thread only, every tick. Samples the lanes of the rails aimed at in
		// the last minute and a half, five times a second, card shown or not: the
		// buildings at their ends and the items that cross them. Returns at once
		// when no rail is watched.
		static void Tick();

		// Drops the cache. Called when the plugin disables.
		static void Reset();
	};
}
