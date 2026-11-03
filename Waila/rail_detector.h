#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SDK { class AActor; class UAuItemDataBase; class UCrBuildingData; }

namespace Waila
{
	// One item on the rail: what the machines attached to it make and take,
	// and what the storage on the network holds of it. Rates are items per
	// minute, each capped by what the rail carries; a negative value means it
	// could not be read, and the card shows "?" for it.
	struct RailRateEntry
	{
		SDK::UAuItemDataBase* itemData = nullptr;
		std::string           displayName;
		float                 perMinMax = -1.0f;   // the makers' output, nobody pulling
		float                 perMinMin = -1.0f;   // left with every consumer downstream running full out
		float                 perMinNow = -1.0f;   // left with the consumers requesting it right now

		// The kinds of building making and taking it, names only, from the
		// game's own Mass data (rail_mass.h): "2x Furnace, Furnace v.2". Empty
		// when not known.
		std::string           from;
		std::string           to;

		// What the storage the network passes through holds of it, summed over
		// those storages, and their name; negative when not read. Storage is
		// neither a maker nor a consumer in the rates.
		int64_t               stored   = -1;
		std::string           storedIn;
	};

	struct RailInfo
	{
		std::string buildingName;
		std::string buildingDesc;
		SDK::UCrBuildingData* buildingData = nullptr;

		// What the rail is set to carry. Empty when it carries anything.
		std::string filterName;

		// What the rail itself carries in items a minute, from its drone speed
		// and spacing; negative when not read.
		float   capacity    = -1.0f;

		// How well the rail is fed right now, the first that holds: Starved when
		// a maker upstream is short of an input, Short when the machines
		// requesting an item want more of it than the rail can bring, Fed when
		// neither and something takes from it, otherwise Unjudged (nothing takes
		// from it, or a figure is unread).
		enum class Feed : uint8_t { Unjudged, Fed, Short, Starved };
		Feed    feed        = Feed::Unjudged;

		int32_t sourceCount = 0;
		bool    truncated   = false;   // the network walk hit a cap
		std::vector<RailRateEntry> rates;   // items made first, then items only in storage

		bool        fromMass  = false;     // the rates come from the game's Mass data
		int32_t     fromCount = 0;         // buildings named in the rates' From and To, by end
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
