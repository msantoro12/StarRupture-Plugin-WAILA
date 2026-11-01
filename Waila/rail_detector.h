#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SDK { class AActor; class UAuItemDataBase; class UCrBuildingData; }

namespace Waila
{
	// One item the rail's sources produce. Rates are items per minute; a negative
	// value means it could not be read, and the card shows "?" for it.
	struct RailRateEntry
	{
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           displayName;
		float                 perMinMax = -1.0f;   // the sources' output, capped by what the rail carries
		float                 perMinMin = -1.0f;   // what is left if every machine ahead takes its full share
		float                 perMinNow = -1.0f;   // what is left given the machines asking for it now
	};

	// The buildings of one kind at an end of the rail's traffic: where an item
	// comes from or goes to, one row for all the buildings of a name making or
	// taking that item. The rate is items per minute, summed over those buildings,
	// negative when not known or not a crafter.
	struct RailPeerEntry
	{
		bool                  source = false;   // false: the item goes there
		std::string           buildingName;
		int32_t               count = 1;   // buildings merged into this row
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           itemName;
		float                 perMinNow = -1.0f;
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
		std::vector<RailRateEntry> rates;

		// From the game's own Mass data (rail_mass.h): the buildings the rail's
		// items come from and go to, loaded or not, shown under `rates`.
		bool                       fromMass = false;
		int32_t                    fromCount = 0;
		int32_t                    destCount = 0;
		std::vector<RailPeerEntry> peers;     // sources first
		std::string                note;      // stands in when there is nothing to list

		bool IsValid() const { return !buildingName.empty(); }
	};

	class RailDetector
	{
	public:
		static bool IsRail(SDK::AActor* actor);

		// Game thread only. The network scan is cached per rail and refreshed about
		// once a second, so calling this every tick is cheap.
		static bool GetRailInfo(SDK::AActor* actor, RailInfo& out);

		// Drops the cache. Called when the plugin disables.
		static void Reset();
	};
}
