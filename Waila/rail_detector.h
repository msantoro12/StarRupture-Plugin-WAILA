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
		float                 perMinNow = -1.0f;
		float                 perMinMax = -1.0f;
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
