#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SDK { class AActor; class UAuItemDataBase; class UCrBuildingData; }

namespace Waila
{
	// The machines of one kind at an end of the rail's network: where an item
	// is made or taken, one row for all the machines of a name making or
	// taking that item. Rates are items per minute, summed over those machines,
	// negative when not known.
	struct RailPeerEntry
	{
		bool                  source = false;   // false: the item goes there
		std::string           buildingName;
		int32_t               count = 1;   // buildings merged into this row
		SDK::UAuItemDataBase* itemData = nullptr;
		float                 perMinFull = -1.0f;   // running full out
		float                 perMinNow  = -1.0f;   // right now: a maker 0 while stalled, a consumer 0 while not requesting
	};

	// One item on the rail: what the machines attached to it make, the
	// machines making and taking it, and what the storage on the network
	// holds of it. Rates are items per minute, each capped by what the rail
	// carries; a negative value means it could not be read, and the card
	// shows "?" for it.
	struct RailRateEntry
	{
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           displayName;
		bool                  made      = true;    // false: nothing attached makes it, only its stock is known
		float                 perMinMax = -1.0f;   // the makers' output, nobody pulling
		float                 perMinMin = -1.0f;   // left with every attached consumer running full out
		float                 perMinNow = -1.0f;   // left with the consumers requesting it right now

		// The machines the rates are summed from, makers first: from the game's
		// own Mass data (rail_mass.h), loaded or not.
		std::vector<RailPeerEntry> peers;

		// What the storage the network passes through holds of it, summed over
		// those storages; negative when not read. Storage is neither a maker
		// nor a consumer in the rates.
		int64_t               stored   = -1;
		int32_t               storages = 0;
	};

	struct RailInfo
	{
		std::string buildingName;
		std::string buildingDesc;
		SDK::UCrBuildingData* buildingData = nullptr;

		// What the rail is set to carry. Empty when it carries anything.
		std::string filterName;

		int32_t sourceCount = 0;
		bool    truncated   = false;   // the network walk hit a cap
		std::vector<RailRateEntry> rates;   // items made first, then items only in stock

		bool        fromMass  = false;     // the rates come from the game's Mass data
		int32_t     fromCount = 0;         // buildings in the rates' peers, by end
		int32_t     destCount = 0;
		std::string note;                  // stands in when there is nothing to list

		bool IsValid() const { return !buildingName.empty(); }
	};

	class RailDetector
	{
	public:
		static bool IsRail(SDK::AActor* actor);

		// Game thread only. The network walk is cached per rail and refreshed
		// about once a second, so calling this every tick is cheap.
		static bool GetRailInfo(SDK::AActor* actor, RailInfo& out);

		// Drops the cache. Called when the plugin disables.
		static void Reset();
	};
}
