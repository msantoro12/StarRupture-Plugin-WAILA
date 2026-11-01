#pragma once

#include <cstdint>
#include <vector>

struct IPluginSelf;
struct IPluginHookScanner;

namespace SDK { class AActor; class UAuItemDataBase; class UAuItemRecipeData; }

// Reads the game's drone logistics from its Mass simulation, which runs for the
// whole world whether or not a building is loaded: a rail's lane holds the
// drones on it, each drone names its request, and the request names the
// building the item comes from and the one it goes to.
//
// Every member of every Mass fragment and every struct stride comes from the
// loader by name (ResolveField). Keys are declared as one feature, "rail
// sources" (Waila::Features::RailSources), so a name the game no longer has
// turns off the rail cards' sources and nothing else.
namespace Waila::RailMass
{
	// A Mass entity, as the engine's FMassEntityHandle packs it: Index in the
	// low half, SerialNumber in the high half. 0 is no entity.
	using Entity = uint64_t;

	// Declares the "rail sources" feature with every key the reader needs, and
	// `alsoNeeds`, the keys of the parts it builds on (the building and item
	// names it shows). Call first in OnPluginLoadHooks, with the other features.
	bool Declare(IPluginSelf* self, IPluginHookScanner* scanner, const std::vector<const char*>& alsoNeeds);

	// Asks the loader for those keys. Call from OnPluginLoadHooks, after
	// Fields::Resolve and the declaration. A miss turns the feature off and
	// leaves the rest of WAILA running.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// True when the feature is on, everything resolved and no read has faulted.
	bool Available();

	// The rail's, or a building's, own Mass entity. 0 when it has none yet.
	Entity EntityOf(SDK::AActor* actor);

	// One source or destination building seen on a drone's request.
	struct Peer
	{
		Entity                 entity = 0;
		bool                   source = false;   // false: the item goes there
		SDK::UAuItemDataBase*  item   = nullptr; // valid only in the tick it was read
	};

	struct Observation
	{
		bool              railFound  = false;   // the rail has a lane fragment
		int32_t           drones     = 0;       // drones on the lane right now
		std::vector<Peer> peers;
	};

	// What a crafter's Mass fragment says. isCrafter is false for storage and
	// stations, which have no crafting fragment.
	struct CrafterState
	{
		bool                    isCrafter = false;
		SDK::UAuItemRecipeData* recipe    = nullptr;   // valid only in the tick it was read
		float                   speed     = 0.0f;
		bool                    stalled   = false;     // missing items or output full
	};

	// Game thread only. False when the reader is unavailable or a read faulted;
	// a fault turns the reader off for the rest of the session.
	bool Observe(Entity rail, Observation& out);
	bool ReadCrafter(Entity building, CrafterState& out);
}
