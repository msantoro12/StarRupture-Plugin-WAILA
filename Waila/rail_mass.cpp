#include "rail_mass.h"
#include "waila_fields.h"
#include "plugin_helpers.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "AuCrafting_classes.hpp"
#include "Chimera_structs.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace SDK;

// Engine layout this file keeps compiled, because the loader has nothing to
// say about it and the engine does not move it between builds:
//   - FMassEntityHandle passed by value: two int32 (Index, SerialNumber), one
//     register. Its size is still asked of the loader (kHandleSize).
//   - TArray: data pointer, then Num as int32 (RawArray).
//   - TSharedPtr: the object pointer is its first word (the entity manager).
//   - A const shared fragment: FConstSharedStruct's first word points at an
//     FStructSharedMemory, whose first word is the struct type; the data
//     follows, aligned to the type's MinAlignment.
//   - The calling convention and argument order of the Mass functions.
// Everything else (every fragment member, every struct size and array stride)
// is asked of the loader by name.

namespace Waila::RailMass
{
	namespace
	{
		struct Handle { int32_t index; int32_t serial; };

		struct RawArray { const uint8_t* data; int32_t num; int32_t max; };

		using GetSubsystemFn = void* (__fastcall*)(const UWorld*);
		using GetFragmentFn  = void* (__fastcall*)(const void* manager, Handle entity, const UScriptStruct* type);
		using EntityCheckFn  = bool  (__fastcall*)(const void* manager, Handle entity);
		using GetHandleFn    = Handle* (__fastcall*)(void* persistentId, Handle* out, const UObject* worldContext);
		using GetSharedFn    = const void* (__fastcall*)(const void* manager, Handle entity, const UScriptStruct* type);

		// The Mass functions, by the names the loader's map would use.
		constexpr const char* kKeyGetSubsystem = "UWorld::GetSubsystem<UMassEntitySubsystem>";
		constexpr const char* kKeyGetFragment  = "FMassEntityManager::InternalGetFragmentDataPtr";
		constexpr const char* kKeyIsValid      = "FMassEntityManager::IsEntityValid";
		constexpr const char* kKeyIsBuilt      = "FMassEntityManager::IsEntityBuilt";
		constexpr const char* kKeyGetHandle    = "FCrMassPersistentEntityID::GetHandle";
		constexpr const char* kKeyGetShared    = "FMassEntityManager::InternalGetConstSharedFragmentPtr";

		// Call sites and function starts in CL-127004, each unique there. The
		// call-site patterns follow the E8 at the offset below to the function.
		constexpr const char* kPatGetSubsystem =
			"49 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 "
			"E8 ?? ?? ?? ?? 48 8B D8 EB ?? 33 DB 48 83 7B 38 00 75 ?? "
			"4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ??";
		constexpr int32_t kAtGetSubsystem = 0x10;

		constexpr const char* kPatGetFragment =
			"E8 ?? ?? ?? ?? 4C 8B C0 48 8B D3 48 8B CE E8 ?? ?? ?? ?? "
			"48 85 C0 0F 84 ?? ?? ?? ?? 0F 10 48 20 66 41 0F 2F 4D 30 "
			"0F 10 40 30 0F 11 4D EF F2 0F 11 45 FF";
		constexpr int32_t kAtGetFragment = 0x0E;

		constexpr const char* kPatGetShared =
			"E8 ?? ?? ?? ?? 4C 8B C0 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 74 ?? "
			"48 8B 10 48 85 D2 74 ?? 48 8B 02 48 0F BF 48 5C 48 8D 41 07 48 F7 D9 48 03 C2 48 23 C1 74 ?? "
			"F3 0F 10 00 48 8B 4D 6F F3 0F 58 01 F3 0F 11 01";
		constexpr int32_t kAtGetShared = 0x0E;

		// IsEntityBuilt is the function start the pattern matches; IsEntityValid
		// is the call 21 bytes in.
		constexpr const char* kPatIsBuilt =
			"48 89 5C 24 18 48 89 54 24 10 57 48 83 EC 40 48 8B DA 48 8B F9 "
			"E8 ?? ?? ?? ?? 84 C0 75 4B 48 8D 0D ?? ?? ?? ?? 85 DB 48 8D 05 ?? ?? ?? ?? "
			"41 B8 45 07 00 00 48 0F 44 C1 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 30 48 8D 15 ?? ?? ?? ??";
		constexpr int32_t kAtIsValid = 21;

		// The function start. It returns the cached handle when both its halves
		// are set; otherwise it looks the ID up in the world's persistent ID
		// subsystem and caches the result in the ID it was called on.
		constexpr const char* kPatGetHandle =
			"48 89 5C 24 08 57 48 83 EC 30 83 39 FF 49 8B C0 48 8B DA 48 8B F9 74 ?? 48 85 C0 75 ??";

		GetSubsystemFn g_getSubsystem = nullptr;
		GetFragmentFn  g_getFragment  = nullptr;
		EntityCheckFn  g_isValid      = nullptr;
		EntityCheckFn  g_isBuilt      = nullptr;
		GetHandleFn    g_getHandle    = nullptr;
		GetSharedFn    g_getShared    = nullptr;

		// A struct's size on the running build, asked by its name. The row's
		// size is filled in by Resolve; 0 means it did not resolve.
		using SizeRow = PluginFieldSpec;
#define RAIL_SIZE_ROW(Name) PluginFieldSpec{ Name, Name, nullptr, PLUGIN_FIELD_FLAG_NONE, 0, -1 }

		SizeRow kHandleSize      = RAIL_SIZE_ROW("FMassEntityHandle");
		SizeRow kPersistentSize  = RAIL_SIZE_ROW("FCrMassPersistentEntityID");
		SizeRow kHelperSize      = RAIL_SIZE_ROW("FCrMassEntityReplicationHelper");
		SizeRow kLineStateSize   = RAIL_SIZE_ROW("FCrLogisticsLineSimulationState");
		SizeRow kLineItemSize    = RAIL_SIZE_ROW("FCrLogisticsLineItem");
		SizeRow kWaitingSize     = RAIL_SIZE_ROW("FCrLogisticsLineWaitingItem");
		SizeRow kSimpleItemSize  = RAIL_SIZE_ROW("FAuSimpleItem");
		SizeRow kRequestSize     = RAIL_SIZE_ROW("FCrLogisticsRequestData");
		SizeRow kRuntimeSize     = RAIL_SIZE_ROW("FCrLogisticsRequestRuntimeData");

		SizeRow* const kSizeRows[] = {
			&kHandleSize, &kPersistentSize, &kHelperSize, &kLineStateSize, &kLineItemSize,
			&kWaitingSize, &kSimpleItemSize, &kRequestSize, &kRuntimeSize,
		};

		// A member: its size is a size row's, or fixed for a pointer, float,
		// bool or TArray header.
		struct Member
		{
			PluginFieldSpec  spec;
			const SizeRow*   type;    // the member's struct, or null
			uint32_t         fixed;   // otherwise its size
			int32_t Offset() const { return spec.offset; }
		};
#define RAIL_MEMBER(Class, Name, Type, Fixed) \
		Member{ PluginFieldSpec{ #Class "::" #Name, #Class, #Name, PLUGIN_FIELD_FLAG_NONE, 0, -1 }, Type, Fixed }

		constexpr uint32_t kPtr    = sizeof(void*);
		constexpr uint32_t kArray  = sizeof(TArray<uint8_t>);
		constexpr uint32_t kFloat  = sizeof(float);
		constexpr uint32_t kBool   = sizeof(bool);
		constexpr uint32_t kByte   = sizeof(uint8_t);

		// An actor's Mass component, and the entity the component made.
		Member mMassComponent  = RAIL_MEMBER(ACrBuildingActorBase, MassActorComponent, nullptr, kPtr);
		Member mEntityHandle   = RAIL_MEMBER(UCrMassActorComponent, EntityHandle, &kHandleSize, 0);

		// A rail's lane: the drones on it.
		Member mLineState      = RAIL_MEMBER(FCrLogisticsLineFragment, State, &kLineStateSize, 0);
		Member mOrderedItems   = RAIL_MEMBER(FCrLogisticsLineSimulationState, OrderedItems, nullptr, kArray);
		Member mWaitingItems   = RAIL_MEMBER(FCrLogisticsLineSimulationState, WaitingItems, nullptr, kArray);
		Member mLineItemEntity = RAIL_MEMBER(FCrLogisticsLineItem, Entity, &kHelperSize, 0);
		Member mWaitingEntity  = RAIL_MEMBER(FCrLogisticsLineWaitingItem, Entity, &kHelperSize, 0);

		// A drone: its request and what it carries.
		Member mRequestUId     = RAIL_MEMBER(FCrLogisticsAgentFragment, RequestUId, &kPersistentSize, 0);
		Member mCarriedItem    = RAIL_MEMBER(FCrLogisticsAgentFragment, CarriedItem, &kSimpleItemSize, 0);
		Member mItemData       = RAIL_MEMBER(FAuSimpleItem, ItemDataBase, nullptr, kPtr);

		// A request: what it wants, and for each drone on it the source and
		// destination building.
		Member mRequestData    = RAIL_MEMBER(FCrLogisticsRequestFragment, Data, &kRequestSize, 0);
		Member mWantedItem     = RAIL_MEMBER(FCrLogisticsRequestData, WantedItem, &kSimpleItemSize, 0);
		Member mRuntimeData    = RAIL_MEMBER(FCrLogisticsRequestData, RuntimeData, nullptr, kArray);
		Member mRuntimeAgent   = RAIL_MEMBER(FCrLogisticsRequestRuntimeData, AgentEntity, &kPersistentSize, 0);
		Member mItemSource     = RAIL_MEMBER(FCrLogisticsRequestRuntimeData, ItemSource, &kPersistentSize, 0);
		Member mItemDest       = RAIL_MEMBER(FCrLogisticsRequestRuntimeData, ItemDestination, &kPersistentSize, 0);

		// A crafter's recipe and state.
		Member mSelectedRecipe = RAIL_MEMBER(FCrCraftingFragment, SelectedRecipe, nullptr, kPtr);
		Member mCurrentRecipe  = RAIL_MEMBER(FCrCraftingFragment, CurrentRecipe, nullptr, kPtr);
		Member mCraftingSpeed  = RAIL_MEMBER(FCrCraftingFragment, CraftingSpeed, nullptr, kFloat);
		Member mMissingItems   = RAIL_MEMBER(FCrCraftingFragment, bIsMissingItems, nullptr, kBool);
		Member mOutputFull     = RAIL_MEMBER(FCrCraftingFragment, bOutputFull, nullptr, kBool);

		// The building name read, a feature of its own: a building entity's building
		// parameters (a const shared fragment) hold the placement data, which names
		// the building whether or not its actor is loaded. Its miss turns off the
		// names and nothing else: the rows then read "Unknown building".
		Member mPlacement      = RAIL_MEMBER(FCrBuildingParameters, PlacementData, nullptr, kPtr);

		Member* const kMembers[] = {
			&mMassComponent, &mEntityHandle, &mLineState, &mOrderedItems, &mWaitingItems,
			&mLineItemEntity, &mWaitingEntity, &mRequestUId, &mCarriedItem, &mItemData, &mRequestData,
			&mWantedItem, &mRuntimeData, &mRuntimeAgent, &mItemSource, &mItemDest, &mSelectedRecipe,
			&mCurrentRecipe, &mCraftingSpeed, &mMissingItems, &mOutputFull,
		};

		// The rail capacity read, a feature of its own: a lane's speed and
		// spacing, whether a route starts on it, and the drone settings' gap for
		// joining it. Its miss turns off the max figure and nothing else.
		Member mLineMoveSpeed  = RAIL_MEMBER(FCrLogisticsLineParameters, MoveSpeed, nullptr, kFloat);
		Member mLineSlotLength = RAIL_MEMBER(FCrLogisticsLineParameters, SlotLength, nullptr, kFloat);
		Member mIsStartLine    = RAIL_MEMBER(FCrLogisticsLineFragment, bIsStartLine, nullptr, kBool);
		Member mHalfSlotLength = RAIL_MEMBER(UDronesDeveloperSettings, HalfRailSlotLengthDefault, nullptr, kFloat);

		Member* const kBuildingMembers[] = { &mPlacement };

		Member* const kCapacityMembers[] = { &mLineMoveSpeed, &mLineSlotLength, &mIsStartLine, &mHalfSlotLength };

		// The machine request read, a feature of its own: what a building asks
		// drones to bring. A building's container lists its requests (ids of
		// request entities) and holds the ones not started yet; each request says
		// whether it wants an item brought (Acquire) or taken away (Store).
		Member mRequestList    = RAIL_MEMBER(FCrLogisticsRequestContainerFragment, Requests, nullptr, kArray);
		Member mPendingList    = RAIL_MEMBER(FCrLogisticsRequestContainerFragment, PendingRequests, nullptr, kArray);
		Member mRequestType    = RAIL_MEMBER(FCrLogisticsRequestData, Type, nullptr, kByte);
		Member mRequestAborted = RAIL_MEMBER(FCrLogisticsRequestData, bAborted, nullptr, kBool);

		Member* const kRequestMembers[] = { &mRequestList, &mPendingList, &mRequestType, &mRequestAborted };

		// The fragments' reflection objects, found by name on first use.
		const UScriptStruct* g_lineStruct     = nullptr;
		const UScriptStruct* g_paramsStruct   = nullptr;
		const UScriptStruct* g_agentStruct    = nullptr;
		const UScriptStruct* g_requestStruct  = nullptr;
		const UScriptStruct* g_craftingStruct = nullptr;
		const UScriptStruct* g_containerStruct = nullptr;
		const UScriptStruct* g_buildingStruct  = nullptr;

		PluginMap::Feature g_feature{ "rail-sources", "Where a drone rail's items come from and go to", {}, {} };
		PluginMap::Feature g_capacityFeature{ "rail-capacity", "How many items a minute a drone rail carries", {}, {} };
		PluginMap::Feature g_requestFeature{ "rail-requests", "What a machine is asking drones to bring", {}, {} };
		PluginMap::Feature g_buildingFeature{ "rail-buildings", "Which building a rail's far ends are", {}, {} };
		bool g_resolved         = false;
		bool g_capacityResolved = false;
		bool g_requestsResolved = false;
		bool g_buildingResolved = false;
		bool g_faulted          = false;
		bool g_capacityFaulted  = false;
		bool g_requestsFaulted  = false;
		bool g_buildingFaulted  = false;

		// Bounds on what one rail can hold, so a corrupt count cannot run away.
		constexpr int32_t kMaxArray  = 4096;
		constexpr size_t  kMaxPeers  = 256;

		bool ResolveSize(IPluginHookScanner* scanner, const IPluginSelf* self, SizeRow& row)
		{
			PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
			request.key       = row.key;
			request.className = row.className;
			request.builtCL   = PLUGIN_GAME_BUILT_CL;
			PluginFieldResult result{};
			if (!scanner->ResolveField(self, &request, &result))
				return false;
			row.size   = result.size;
			row.offset = 0;
			return true;
		}

		uintptr_t ResolveFunction(IPluginHookScanner* scanner, const IPluginSelf* self, const char* key,
			const char* pattern, int32_t followAt)
		{
			return PluginMap::Function(scanner, self, key, pattern, PLUGIN_SCAN_FUNCTION_START,
				followAt >= 0 ? PLUGIN_SCAN_FLAG_FOLLOW_REL32 : 0u, 0, followAt >= 0 ? followAt : 0);
		}

		Handle ToHandle(Entity e)
		{
			Handle h;
			std::memcpy(&h, &e, sizeof(h));
			return h;
		}

		Entity ToEntity(Handle h)
		{
			Entity e;
			std::memcpy(&e, &h, sizeof(e));
			return e;
		}

		template <class T>
		T ReadAt(const uint8_t* base, int32_t offset)
		{
			T value{};
			std::memcpy(&value, base + offset, sizeof(T));
			return value;
		}

		// A persistent id's entity, resolved the way the game resolves it. Its
		// cached handle is transient and stays empty until the game asks for that
		// copy of the id, so most ids in the Mass data have none. The lookup runs
		// on a copy, so the game's data is left as it was. 0 = no entity.
		Entity HandleOf(const uint8_t* persistentId, const UWorld* world)
		{
			uint8_t copy[32]{};
			if (!world || kPersistentSize.size > sizeof(copy))
				return 0;
			std::memcpy(copy, persistentId, kPersistentSize.size);
			Handle h{};
			g_getHandle(copy, &h, world);
			return h.index > 0 ? ToEntity(h) : 0;
		}

		RawArray ArrayAt(const uint8_t* base, int32_t offset)
		{
			RawArray a = ReadAt<RawArray>(base, offset);
			if (!a.data || a.num < 0 || a.num > kMaxArray)
				return RawArray{ nullptr, 0, 0 };
			return a;
		}

		const UScriptStruct* FindStruct(const char* name)
		{
			return UObject::FindObjectFast<UScriptStruct>(name);
		}

		bool FindStructs()
		{
			if (!g_lineStruct)     g_lineStruct     = FindStruct("CrLogisticsLineFragment");
			if (!g_agentStruct)    g_agentStruct    = FindStruct("CrLogisticsAgentFragment");
			if (!g_requestStruct)  g_requestStruct  = FindStruct("CrLogisticsRequestFragment");
			if (!g_craftingStruct) g_craftingStruct = FindStruct("CrCraftingFragment");
			return g_lineStruct && g_agentStruct && g_requestStruct && g_craftingStruct;
		}

		const UScriptStruct* Found(const UScriptStruct*& cache, const char* name)
		{
			if (!cache)
				cache = FindStruct(name);
			return cache;
		}

		const void* Manager(const UWorld* world)
		{
			if (!world)
				return nullptr;
			const uint8_t* subsystem = static_cast<const uint8_t*>(g_getSubsystem(world));
			return PluginMap::MassEntityManagerOf(subsystem);
		}

		// A built, current entity's fragment, or null.
		const uint8_t* Fragment(const void* manager, Entity entity, const UScriptStruct* type)
		{
			if (!entity || !type)
				return nullptr;
			const Handle h = ToHandle(entity);
			if (!g_isValid(manager, h) || !g_isBuilt(manager, h))
				return nullptr;
			return static_cast<const uint8_t*>(g_getFragment(manager, h, type));
		}

		UAuItemDataBase* ItemOf(const uint8_t* simpleItem)
		{
			auto* item = ReadAt<UAuItemDataBase*>(simpleItem, mItemData.Offset());
			return item && UKismetSystemLibrary::IsValid(item) ? item : nullptr;
		}

		void AddDrones(const uint8_t* state, int32_t arrayOffset, uint32_t stride, int32_t entityOffset,
			const UWorld* world, std::vector<Entity>& drones)
		{
			const RawArray items = ArrayAt(state, arrayOffset);
			for (int32_t i = 0; i < items.num; ++i)
			{
				const uint8_t* element = items.data + static_cast<size_t>(i) * stride;
				const Entity drone = HandleOf(element + entityOffset + Fields::HelperEntity.offset, world);
				if (drone && drones.size() < kMaxPeers)
					drones.push_back(drone);
			}
		}

		void ObserveImpl(Entity rail, Observation& out)
		{
			if (!FindStructs())
				return;
			const UWorld* world = PluginSdkRuntime::GetWorld();
			const void* manager = Manager(world);
			const uint8_t* line = manager ? Fragment(manager, rail, g_lineStruct) : nullptr;
			if (!line)
				return;
			out.railFound = true;

			std::vector<Entity> drones;
			const uint8_t* state = line + mLineState.Offset();
			AddDrones(state, mOrderedItems.Offset(), kLineItemSize.size, mLineItemEntity.Offset(), world, drones);
			AddDrones(state, mWaitingItems.Offset(), kWaitingSize.size, mWaitingEntity.Offset(), world, drones);

			for (Entity drone : drones)
			{
				const uint8_t* agent = Fragment(manager, drone, g_agentStruct);
				if (!agent)
					continue;
				++out.drones;

				const Entity requestEntity = HandleOf(agent + mRequestUId.Offset(), world);
				const uint8_t* request = Fragment(manager, requestEntity, g_requestStruct);
				if (!request)
					continue;
				const uint8_t* data = request + mRequestData.Offset();

				// What the drone carries, or when it is still on its way to pick
				// up, what the request wants.
				UAuItemDataBase* item = ItemOf(agent + mCarriedItem.Offset());
				const bool carried = item != nullptr;
				if (!item)
					item = ItemOf(data + mWantedItem.Offset());

				const RawArray runtime = ArrayAt(data, mRuntimeData.Offset());
				for (int32_t i = 0; i < runtime.num; ++i)
				{
					const uint8_t* entry = runtime.data + static_cast<size_t>(i) * kRuntimeSize.size;
					// A request can serve several drones; the entry that names this
					// one is its own. A request with a single entry is this drone's.
					if (runtime.num > 1 && HandleOf(entry + mRuntimeAgent.Offset(), world) != drone)
						continue;
					const Entity source = HandleOf(entry + mItemSource.Offset(), world);
					const Entity dest   = HandleOf(entry + mItemDest.Offset(), world);
					if (source && out.peers.size() < kMaxPeers)
						out.peers.push_back(Peer{ source, true, item, carried });
					if (dest && out.peers.size() < kMaxPeers)
						out.peers.push_back(Peer{ dest, false, item, carried });
				}
			}
		}

		void ReadCrafterImpl(Entity building, CrafterState& out)
		{
			if (!FindStructs())
				return;
			const void* manager = Manager(PluginSdkRuntime::GetWorld());
			const uint8_t* fragment = manager ? Fragment(manager, building, g_craftingStruct) : nullptr;
			if (!fragment)
				return;

			out.isCrafter = true;
			auto* recipe = ReadAt<UAuItemRecipeData*>(fragment, mSelectedRecipe.Offset());
			if (!recipe)
				recipe = ReadAt<UAuItemRecipeData*>(fragment, mCurrentRecipe.Offset());
			out.recipe  = recipe && UKismetSystemLibrary::IsValid(recipe) ? recipe : nullptr;
			out.speed   = ReadAt<float>(fragment, mCraftingSpeed.Offset());
			out.stalled = ReadAt<bool>(fragment, mMissingItems.Offset()) || ReadAt<bool>(fragment, mOutputFull.Offset());
		}

		// A built, current entity's const shared fragment, or null. The address
		// is worked out the way the game's GetConstSharedFragmentDataPtr does.
		const uint8_t* SharedFragment(const void* manager, Entity entity, const UScriptStruct* type)
		{
			if (!entity || !type)
				return nullptr;
			const Handle h = ToHandle(entity);
			if (!g_isValid(manager, h) || !g_isBuilt(manager, h))
				return nullptr;
			const auto* shared = static_cast<const uint8_t* const*>(g_getShared(manager, h, type));
			const uint8_t* memory = shared ? *shared : nullptr;
			const auto* memoryType = memory ? ReadAt<const UStruct*>(memory, 0) : nullptr;
			if (!memoryType || memoryType->MinAlignment <= 0)
				return nullptr;
			const uintptr_t align = static_cast<uintptr_t>(memoryType->MinAlignment);
			return reinterpret_cast<const uint8_t*>(
				(reinterpret_cast<uintptr_t>(memory) + sizeof(void*) - 1 + align) & ~(align - 1));
		}

		// The lane's throughput, the way UCrLogisticsLineSimulateMovementProcessor
		// moves drones along it. Each tick a drone moves MoveSpeed x dt and stops
		// SlotLength behind the drone ahead. A waiting drone joins once the space
		// left at the start (the lane's length, less the last drone's distance and
		// half a SlotLength, or nothing on a start lane) reaches the settings'
		// HalfRailSlotLengthDefault. A drone arriving while none wait may join
		// sooner, but once drones queue, which is when the rail is full, they join
		// the second way. So a full rail runs drones no closer than the larger of
		// the two gaps. The request subsystem loads a drone with at most one item
		// (UpdateRequests clamps the carried count to 1), whatever the item.
		void ReadCapacityImpl(Entity rail, float& perMin)
		{
			if (!FindStructs() || !Found(g_paramsStruct, "CrLogisticsLineParameters"))
				return;
			const void* manager = Manager(PluginSdkRuntime::GetWorld());
			const uint8_t* line   = manager ? Fragment(manager, rail, g_lineStruct) : nullptr;
			const uint8_t* params = line ? SharedFragment(manager, rail, g_paramsStruct) : nullptr;
			const auto* settings  = reinterpret_cast<const uint8_t*>(UDronesDeveloperSettings::GetDefaultObj());
			if (!params || !settings)
				return;

			const float speed     = ReadAt<float>(params, mLineMoveSpeed.Offset());
			const float slot      = ReadAt<float>(params, mLineSlotLength.Offset());
			const float joinSpace = ReadAt<float>(settings, mHalfSlotLength.Offset());
			const bool  startLine = ReadAt<bool>(line, mIsStartLine.Offset());

			const float joinGap = (startLine ? 0.0f : 0.5f * slot) + joinSpace;
			const float gap     = slot > joinGap ? slot : joinGap;
			if (speed > 0.0f && gap > 0.0f && std::isfinite(speed / gap))
				perMin = speed / gap * 60.0f;
		}

		// The building a built entity is: the placement data in its building
		// parameters, when that is building data.
		void ReadBuildingImpl(Entity building, UCrBuildingData*& out)
		{
			if (!Found(g_buildingStruct, "CrBuildingParameters"))
				return;
			const void* manager = Manager(PluginSdkRuntime::GetWorld());
			const uint8_t* params = manager ? SharedFragment(manager, building, g_buildingStruct) : nullptr;
			if (!params)
				return;

			auto* placement = ReadAt<UObject*>(params, mPlacement.Offset());
			if (placement && UKismetSystemLibrary::IsValid(placement) && placement->IsA(UCrBuildingData::StaticClass()))
				out = static_cast<UCrBuildingData*>(placement);
		}

		// An open request that wants an item brought: not aborted, of type Acquire.
		void AddWanted(const uint8_t* request, std::vector<UAuItemDataBase*>& wanted)
		{
			if (ReadAt<bool>(request, mRequestAborted.Offset())
				|| ReadAt<uint8_t>(request, mRequestType.Offset()) != static_cast<uint8_t>(ECrLogisticsRequestType::Acquire))
				return;
			UAuItemDataBase* item = ItemOf(request + mWantedItem.Offset());
			if (item && std::find(wanted.begin(), wanted.end(), item) == wanted.end())
				wanted.push_back(item);
		}

		// What a building asks the drones to bring, from the request container on
		// its Mass entity: the requests it has made (by id) and the ones waiting
		// to start. A building with no container leaves `known` false.
		void ReadRequestsImpl(Entity building, Requests& out)
		{
			const UWorld* world = PluginSdkRuntime::GetWorld();
			if (!FindStructs() || !Found(g_containerStruct, "CrLogisticsRequestContainerFragment") || !world)
				return;
			const void* manager = Manager(world);
			const uint8_t* container = manager ? Fragment(manager, building, g_containerStruct) : nullptr;
			if (!container)
				return;
			out.known = true;

			const RawArray made = ArrayAt(container, mRequestList.Offset());
			for (int32_t i = 0; i < made.num; ++i)
			{
				const Entity entity = HandleOf(made.data + static_cast<size_t>(i) * kPersistentSize.size, world);
				const uint8_t* request = Fragment(manager, entity, g_requestStruct);
				if (request)
					AddWanted(request + mRequestData.Offset(), out.wanted);
			}

			const RawArray waiting = ArrayAt(container, mPendingList.Offset());
			for (int32_t i = 0; i < waiting.num; ++i)
				AddWanted(waiting.data + static_cast<size_t>(i) * kRequestSize.size, out.wanted);
		}

		// A bad pointer in the game's data must not take the game down.
		// A fault turns off the reads that share `faulted`; the capacity and the
		// request reads keep their own, so one of them failing leaves the rest.
		template <class Fn>
		bool Guarded(Fn&& fn, bool& faulted = g_faulted)
		{
			struct Call
			{
				static bool Run(void* ctx, void (*thunk)(void*), DWORD* code)
				{
					__try
					{
						thunk(ctx);
						return true;
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						*code = GetExceptionCode();
						return false;
					}
				}
			};
			DWORD code = 0;
			const bool ok = Call::Run(&fn, [](void* ctx) { (*static_cast<Fn*>(ctx))(); }, &code);
			if (!ok)
			{
				faulted = true;
				LOG_WARN("RailMass: exception 0x%08lX while reading the Mass data; this read is off for the session", code);
			}
			return ok;
		}
	}

	bool Declare(IPluginSelf* self, IPluginHookScanner* scanner, const std::vector<const char*>& alsoNeeds)
	{
		// The keys every rail read starts from. The capacity and request reads
		// list them too, so their own misses turn off only themselves while a
		// miss here turns off all three.
		std::vector<const char*> core;
		for (const SizeRow* row : kSizeRows)
			core.push_back(row->key);
		for (const Member* member : kMembers)
			core.push_back(member->spec.key);
		core.push_back(Fields::HelperEntity.key);
		core.push_back(PluginMap::MassEntityManagerField.key);
		core.push_back(kKeyGetSubsystem);
		core.push_back(kKeyGetFragment);
		core.push_back(kKeyIsValid);
		core.push_back(kKeyIsBuilt);
		core.push_back(kKeyGetHandle);

		g_feature.keys = core;
		g_feature.keys.insert(g_feature.keys.end(), alsoNeeds.begin(), alsoNeeds.end());

		g_capacityFeature.keys = core;
		for (const Member* member : kCapacityMembers)
			g_capacityFeature.keys.push_back(member->spec.key);

		g_capacityFeature.keys.push_back(kKeyGetShared);

		g_buildingFeature.keys = core;
		for (const Member* member : kBuildingMembers)
			g_buildingFeature.keys.push_back(member->spec.key);
		g_buildingFeature.keys.push_back(kKeyGetShared);

		g_requestFeature.keys = core;
		for (const Member* member : kRequestMembers)
			g_requestFeature.keys.push_back(member->spec.key);

		return PluginMap::DeclareFeatures(scanner, self, g_feature, g_capacityFeature, g_requestFeature, g_buildingFeature);
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		// A member, once the size it is checked against is known. A miss is
		// recorded by the loader and turns off the feature that lists it. The
		// struct sizes are resolved first, so a member whose struct did not
		// resolve is skipped here; that miss is already recorded.
		auto resolveMember = [&](Member& member)
		{
			const uint32_t size = member.type ? member.type->size : member.fixed;
			if (!size)
				return false;
			member.spec.size = size;
			return PluginMap::ResolveField(scanner, self, member.spec);
		};

		// Every key is asked even after a miss, so the log lists them all.
		bool all = true;
		for (SizeRow* row : kSizeRows)
			all = ResolveSize(scanner, self, *row) && all;

		for (Member* member : kMembers)
			all = resolveMember(*member) && all;
		all = PluginMap::ResolveMassEntityManager(scanner, self) && all;

		bool capacity = true;
		for (Member* member : kCapacityMembers)
			capacity = resolveMember(*member) && capacity;
		bool building = true;
		for (Member* member : kBuildingMembers)
			building = resolveMember(*member) && building;
		bool requests = true;
		for (Member* member : kRequestMembers)
			requests = resolveMember(*member) && requests;

		const uintptr_t getSubsystem = ResolveFunction(scanner, self, kKeyGetSubsystem, kPatGetSubsystem, kAtGetSubsystem);
		const uintptr_t getFragment  = ResolveFunction(scanner, self, kKeyGetFragment, kPatGetFragment, kAtGetFragment);
		const uintptr_t isValid      = ResolveFunction(scanner, self, kKeyIsValid, kPatIsBuilt, kAtIsValid);
		const uintptr_t isBuilt      = ResolveFunction(scanner, self, kKeyIsBuilt, kPatIsBuilt, -1);
		const uintptr_t getHandle    = ResolveFunction(scanner, self, kKeyGetHandle, kPatGetHandle, -1);
		const uintptr_t getShared    = ResolveFunction(scanner, self, kKeyGetShared, kPatGetShared, kAtGetShared);
		g_getSubsystem = reinterpret_cast<GetSubsystemFn>(getSubsystem);
		g_getFragment  = reinterpret_cast<GetFragmentFn>(getFragment);
		g_isValid      = reinterpret_cast<EntityCheckFn>(isValid);
		g_isBuilt      = reinterpret_cast<EntityCheckFn>(isBuilt);
		g_getHandle    = reinterpret_cast<GetHandleFn>(getHandle);
		g_getShared    = reinterpret_cast<GetSharedFn>(getShared);

		g_resolved         = all && getSubsystem && getFragment && isValid && isBuilt && getHandle;
		g_capacityResolved = capacity && getShared;
		g_buildingResolved = building && getShared;
		g_requestsResolved = requests;
		if (!g_resolved || !g_feature.On())
			LOG_WARN("RailMass: rail sources are off on this build; the rest of WAILA is unaffected");
		else if (!g_capacityResolved || !g_capacityFeature.On())
			LOG_WARN("RailMass: the rail capacity is off on this build; a rail shows no max, the rest of its card is unaffected");
		if (g_resolved && g_feature.On() && (!g_requestsResolved || !g_requestFeature.On()))
			LOG_WARN("RailMass: machine requests are off on this build; a rail shows no now figure, the rest of its card is unaffected");
		if (g_resolved && g_feature.On() && (!g_buildingResolved || !g_buildingFeature.On()))
			LOG_WARN("RailMass: building names are off on this build; a rail's ends read Unknown building, the rest of its card is unaffected");
	}

	bool Available()
	{
		return g_resolved && g_feature.On() && !g_faulted
			&& g_getSubsystem && g_getFragment && g_isValid && g_isBuilt && g_getHandle
			&& Fields::HelperEntity.offset >= 0;
	}

	bool CapacityAvailable()
	{
		return Available() && !g_capacityFaulted && g_capacityResolved && g_capacityFeature.On() && g_getShared;
	}

	bool BuildingAvailable()
	{
		return Available() && !g_buildingFaulted && g_buildingResolved && g_buildingFeature.On() && g_getShared;
	}

	bool RequestsAvailable()
	{
		return Available() && !g_requestsFaulted && g_requestsResolved && g_requestFeature.On();
	}

	Entity EntityOf(AActor* actor)
	{
		if (!Available() || !actor || !UKismetSystemLibrary::IsValid(actor))
			return 0;
		const auto* component = ReadAt<const UObject*>(reinterpret_cast<const uint8_t*>(actor), mMassComponent.Offset());
		if (!component || !UKismetSystemLibrary::IsValid(component))
			return 0;
		const Entity e = ReadAt<Entity>(reinterpret_cast<const uint8_t*>(component), mEntityHandle.Offset());
		return ToHandle(e).index > 0 ? e : 0;
	}

	bool Observe(Entity rail, Observation& out)
	{
		if (!Available())
			return false;
		return Guarded([&] { ObserveImpl(rail, out); });
	}

	bool ReadCrafter(Entity building, CrafterState& out)
	{
		if (!Available())
			return false;
		return Guarded([&] { ReadCrafterImpl(building, out); });
	}

	bool ReadBuilding(Entity building, UCrBuildingData*& out)
	{
		out = nullptr;
		if (!BuildingAvailable())
			return false;
		return Guarded([&] { ReadBuildingImpl(building, out); }, g_buildingFaulted) && out != nullptr;
	}

	bool ReadCapacity(Entity rail, float& perMin)
	{
		if (!CapacityAvailable())
			return false;
		return Guarded([&] { ReadCapacityImpl(rail, perMin); }, g_capacityFaulted);
	}

	bool ReadRequests(Entity building, Requests& out)
	{
		if (!RequestsAvailable())
			return false;
		return Guarded([&] { ReadRequestsImpl(building, out); }, g_requestsFaulted);
	}
}
