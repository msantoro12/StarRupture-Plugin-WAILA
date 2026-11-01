#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuActorPlacement_classes.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"
#include "BP_CoolerActive_classes.hpp"
#include "BP_CoolerPassive_classes.hpp"
#include "BP_PackageSender_classes.hpp"
#include "plugin_map.h"
#include "plugin_helpers.h"

#include <string>

// Every member of a game class WAILA reads, taken from the loader's map for the
// running game build (see plugin_map.h). Resolved once in OnPluginLoadHooks by
// Waila::Functions::Resolve; read on the game thread with In(object), which is
// null when the object is null.
//
// Structs WAILA only steps through or copies (an array's elements, a block the
// game hands back) are covered by a size check instead of a field per member.

#define WAILA_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace Waila::Fields
{
	// Buildings
	inline auto PlacementData    = WAILA_FIELD(ACrBuildingActorBase, PlacementData);
	inline auto ItemStorage      = WAILA_FIELD(ACrBuildingActorBase, ItemStorage);
	inline auto InItemStorage    = WAILA_FIELD(ACrBuildingActorBase, InItemStorage);
	inline auto BuildingName     = WAILA_FIELD(UCrBuildingData, BuildingName);
	inline auto BuildingDesc     = WAILA_FIELD(UCrBuildingData, BuildingDescription);

	// Items
	inline auto ItemName         = WAILA_FIELD(UAuItemDataBase, ItemName);
	inline auto UniqueItemName   = WAILA_FIELD(UAuItemDataBase, UniqueItemName);
	inline auto GridColumns      = WAILA_FIELD(UCrBuildingItemStorageComponent, GridColumns);
	inline auto GridRows         = WAILA_FIELD(UCrBuildingItemStorageComponent, GridRows);

	// Crafters
	inline auto CraftComponent   = WAILA_FIELD(ACrCrafter, CraftComponent);
	inline auto CraftingSettings = WAILA_FIELD(ACrCrafter, CraftingSettings);
	inline auto ItemsToCraft     = WAILA_FIELD(UCrCraftingComponent, ItemsToCraft);
	inline auto NeededResources  = WAILA_FIELD(UAuItemRecipeData, NeededResources);
	inline auto RecipeOutputItem = WAILA_FIELD(UAuItemRecipeData, OutputItem);
	inline auto RecipeBuildTime  = WAILA_FIELD(UAuItemRecipeData, BuildTime);

	// Drone rails
	inline auto SocketsData      = WAILA_FIELD(ACrDronePathPoint, SocketsData);
	inline auto Spline           = WAILA_FIELD(ACrBuildingSplineActorBase, Spline);

	// Package transport. SendingTime has no member in the Game-SDK, so it is a
	// native field: the loader's row for the running build, or none and the
	// plugin is refused.
	inline auto PackageTransportReplicator = WAILA_FIELD(ACrGameStateBase, PackageTransportReplicator);
	inline auto SendingTime = PluginField<float>{ { "ACrPackageTransportReplicator::SendingTime",
		"ACrPackageTransportReplicator", "SendingTime", PLUGIN_FIELD_NATIVE, sizeof(float), 0x2C0 } };
	inline auto SenderReceiversContainer = WAILA_FIELD(ACrPackageTransportReplicator, SenderReceiversContainer);
	inline auto AllSenderReceiversData   = WAILA_FIELD(FCrSenderReceiversContainer, AllSenderReceiversData);
	inline auto ConnectionsContainer     = WAILA_FIELD(ACrPackageTransportReplicator, ConnectionsContainer);
	inline auto ConnectionsData          = WAILA_FIELD(FCrPackageTransportConnectionsContainer, ConnectionsData);
	inline auto ConnectionSender         = WAILA_FIELD(FCrPackageTransportConnectionData, Sender);
	inline auto ConnectionItem           = WAILA_FIELD(FCrPackageTransportConnectionData, Item);
	inline auto ConnectionStartSendTime  = WAILA_FIELD(FCrPackageTransportConnectionData, StartSendServerTime);
	inline auto HelperNetID              = WAILA_FIELD(FCrMassEntityReplicationHelper, NetID);
	inline auto HelperEntity             = WAILA_FIELD(FCrMassEntityReplicationHelper, Entity);
	inline auto PlacementSockets         = WAILA_FIELD(UAuActorPlacementSocketsComponent, Sockets);

	// Blueprint (_C) members have no compiled offset: found by name on first use.
	inline PluginLazyField<SDK::UAuActorPlacementSocketsComponent*> ActiveCoolerSockets{ "AuActorPlacementSockets" };
	inline PluginLazyField<SDK::UAuActorPlacementSocketsComponent*> PassiveCoolerSockets{ "AuActorPlacementSockets" };
	inline PluginLazyField<bool> SenderCanSend{ "CanSend" };

	// Structs read as array elements or through a pointer the game hands back.
	inline auto StorageContainerSize   = SR_SIZE_CHECK(FCrItemsStorageContainer);
	inline auto StorageItemSize        = SR_SIZE_CHECK(FCrStorageItem);
	inline auto CraftItemSize          = SR_SIZE_CHECK(FAuCraftItem);
	inline auto RecipeOrderSize        = SR_SIZE_CHECK(FAuItemRecipeOrder);
	inline auto CraftingFragmentSize   = SR_SIZE_CHECK(FCrCraftingFragment);
	inline auto CraftingSettingsSize   = SR_SIZE_CHECK(FCrCraftingSettings);
	inline auto SocketDataSize         = SR_SIZE_CHECK(FBuildingSocketData);
	inline auto PlacementSocketSize    = SR_SIZE_CHECK(FAuActorPlacementSingleSocketData);
	inline auto SenderReceiverSize     = SR_SIZE_CHECK(FCrSenderReceiverData);
	inline auto ConnectionDataSize     = SR_SIZE_CHECK(FCrPackageTransportConnectionData);

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// The member's value, or a default when the member did not resolve.
	template <class T, class Object>
	T ValueOr(const PluginField<T>& field, const Object* object, T fallback = T{})
	{
		const T* at = field.In(static_cast<const void*>(object));
		return at ? *at : fallback;
	}

	// The loader's property reader for Blueprint members, or null.
	inline const IPluginObjectProperties* ObjectProperties()
	{
		IPluginHooks* hooks = GetHooks();
		return hooks ? hooks->ObjectProperties : nullptr;
	}

	// The building's UCrBuildingData, or null.
	inline SDK::UCrBuildingData* BuildingDataOf(SDK::ACrBuildingActorBase* building)
	{
		SDK::UAuActorPlacementData* placement = ValueOr<SDK::UAuActorPlacementData*>(PlacementData, building, nullptr);
		return placement && placement->IsA(SDK::UCrBuildingData::StaticClass())
			? static_cast<SDK::UCrBuildingData*>(placement)
			: nullptr;
	}

	// The localized name and description of a building. False without data.
	inline bool ReadBuildingTexts(SDK::UCrBuildingData* data, std::string& name, std::string& description)
	{
		const SDK::FText* nameText = BuildingName.In(data);
		const SDK::FText* descText = BuildingDesc.In(data);
		if (!nameText || !descText)
			return false;

		name        = SDK::UKismetTextLibrary::Conv_TextToString(*nameText).ToString();
		description = SDK::UKismetTextLibrary::Conv_TextToString(*descText).ToString();
		return true;
	}

	// An item's unique name and its localized name. Empty when unresolved.
	inline std::string UniqueNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FName* name = UniqueItemName.In(static_cast<const void*>(item));
		return name ? name->ToString() : std::string();
	}

	inline std::string DisplayNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FText* text = ItemName.In(static_cast<const void*>(item));
		return text ? SDK::UKismetTextLibrary::Conv_TextToString(*text).ToString() : std::string();
	}
}
