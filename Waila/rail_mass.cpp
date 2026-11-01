#include "rail_mass.h"
#include "waila_fields.h"
#include "plugin_helpers.h"

#include "Basic.hpp"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "AuCrafting_classes.hpp"

#include <windows.h>
#include <cstring>
#include <vector>

using namespace SDK;

// Engine layout this file keeps compiled, because the loader has nothing to
// say about it and the engine does not move it between builds:
//   - FMassEntityHandle passed by value: two int32 (Index, SerialNumber), one
//     register. Its size is still asked of the loader (kHandleSize).
//   - TArray: data pointer, then Num as int32 (RawArray).
//   - TSharedPtr: the object pointer is its first word (the entity manager).
//   - The calling convention and argument order of the three Mass functions.
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

		// The three functions, by the names the loader's map would use.
		constexpr const char* kKeyGetSubsystem = "UWorld::GetSubsystem<UMassEntitySubsystem>";
		constexpr const char* kKeyGetFragment  = "FMassEntityManager::InternalGetFragmentDataPtr";
		constexpr const char* kKeyIsValid      = "FMassEntityManager::IsEntityValid";
		constexpr const char* kKeyIsBuilt      = "FMassEntityManager::IsEntityBuilt";

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

		// IsEntityBuilt is the function start the pattern matches; IsEntityValid
		// is the call 21 bytes in.
		constexpr const char* kPatIsBuilt =
			"48 89 5C 24 18 48 89 54 24 10 57 48 83 EC 40 48 8B DA 48 8B F9 "
			"E8 ?? ?? ?? ?? 84 C0 75 4B 48 8D 0D ?? ?? ?? ?? 85 DB 48 8D 05 ?? ?? ?? ?? "
			"41 B8 45 07 00 00 48 0F 44 C1 4C 8D 0D ?? ?? ?? ?? 48 89 44 24 30 48 8D 15 ?? ?? ?? ??";
		constexpr int32_t kAtIsValid = 21;

		GetSubsystemFn g_getSubsystem = nullptr;
		GetFragmentFn  g_getFragment  = nullptr;
		EntityCheckFn  g_isValid      = nullptr;
		EntityCheckFn  g_isBuilt      = nullptr;

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

		// An actor's Mass component, and the entity the component made.
		Member mMassComponent  = RAIL_MEMBER(ACrBuildingActorBase, MassActorComponent, nullptr, kPtr);
		Member mEntityHandle   = RAIL_MEMBER(UCrMassActorComponent, EntityHandle, &kHandleSize, 0);
		Member mCachedHandle   = RAIL_MEMBER(FCrMassPersistentEntityID, CachedHandle, &kHandleSize, 0);

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

		Member* const kMembers[] = {
			&mMassComponent, &mEntityHandle, &mCachedHandle, &mLineState, &mOrderedItems, &mWaitingItems,
			&mLineItemEntity, &mWaitingEntity, &mRequestUId, &mCarriedItem, &mItemData, &mRequestData,
			&mWantedItem, &mRuntimeData, &mRuntimeAgent, &mItemSource, &mItemDest, &mSelectedRecipe,
			&mCurrentRecipe, &mCraftingSpeed, &mMissingItems, &mOutputFull,
		};

		// The three fragments' reflection objects, found by name on first use.
		const UScriptStruct* g_lineStruct     = nullptr;
		const UScriptStruct* g_agentStruct    = nullptr;
		const UScriptStruct* g_requestStruct  = nullptr;
		const UScriptStruct* g_craftingStruct = nullptr;

		PluginMap::Feature g_feature{ "rail-sources", "Where a drone rail's items come from and go to", {}, {} };
		bool g_resolved = false;
		bool g_faulted  = false;

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

		// A persistent id's cached handle; 0 when the id was never resolved.
		Entity CachedOf(const uint8_t* persistentId)
		{
			const Entity e = ReadAt<Entity>(persistentId, mCachedHandle.Offset());
			return ToHandle(e).index > 0 ? e : 0;
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

		const void* Manager()
		{
			UWorld* world = PluginSdkRuntime::GetWorld();
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
			std::vector<Entity>& drones)
		{
			const RawArray items = ArrayAt(state, arrayOffset);
			for (int32_t i = 0; i < items.num; ++i)
			{
				const uint8_t* element = items.data + static_cast<size_t>(i) * stride;
				const Entity drone = CachedOf(element + entityOffset + Fields::HelperEntity.offset);
				if (drone && drones.size() < kMaxPeers)
					drones.push_back(drone);
			}
		}

		void ObserveImpl(Entity rail, Observation& out)
		{
			if (!FindStructs())
				return;
			const void* manager = Manager();
			const uint8_t* line = manager ? Fragment(manager, rail, g_lineStruct) : nullptr;
			if (!line)
				return;
			out.railFound = true;

			std::vector<Entity> drones;
			const uint8_t* state = line + mLineState.Offset();
			AddDrones(state, mOrderedItems.Offset(), kLineItemSize.size, mLineItemEntity.Offset(), drones);
			AddDrones(state, mWaitingItems.Offset(), kWaitingSize.size, mWaitingEntity.Offset(), drones);

			for (Entity drone : drones)
			{
				const uint8_t* agent = Fragment(manager, drone, g_agentStruct);
				if (!agent)
					continue;
				++out.drones;

				const Entity requestEntity = CachedOf(agent + mRequestUId.Offset());
				const uint8_t* request = Fragment(manager, requestEntity, g_requestStruct);
				if (!request)
					continue;
				const uint8_t* data = request + mRequestData.Offset();

				// What the drone carries, or when it is still on its way to pick
				// up, what the request wants.
				UAuItemDataBase* item = ItemOf(agent + mCarriedItem.Offset());
				if (!item)
					item = ItemOf(data + mWantedItem.Offset());

				const RawArray runtime = ArrayAt(data, mRuntimeData.Offset());
				for (int32_t i = 0; i < runtime.num; ++i)
				{
					const uint8_t* entry = runtime.data + static_cast<size_t>(i) * kRuntimeSize.size;
					// A request can serve several drones; the entry that names this
					// one is its own. A request with a single entry is this drone's.
					if (runtime.num > 1 && CachedOf(entry + mRuntimeAgent.Offset()) != drone)
						continue;
					const Entity source = CachedOf(entry + mItemSource.Offset());
					const Entity dest   = CachedOf(entry + mItemDest.Offset());
					if (source && out.peers.size() < kMaxPeers)
						out.peers.push_back(Peer{ source, true, item });
					if (dest && out.peers.size() < kMaxPeers)
						out.peers.push_back(Peer{ dest, false, item });
				}
			}
		}

		void ReadCrafterImpl(Entity building, CrafterState& out)
		{
			if (!FindStructs())
				return;
			const void* manager = Manager();
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

		// A bad pointer in the game's data must not take the game down.
		template <class Fn>
		bool Guarded(Fn&& fn)
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
				g_faulted = true;
				LOG_WARN("RailMass: exception 0x%08lX while reading the Mass data; rail sources are off for this session", code);
			}
			return ok;
		}
	}

	bool Declare(IPluginSelf* self, IPluginHookScanner* scanner, const std::vector<const char*>& alsoNeeds)
	{
		std::vector<const char*>& keys = g_feature.keys;
		keys.clear();
		for (const SizeRow* row : kSizeRows)
			keys.push_back(row->key);
		for (const Member* member : kMembers)
			keys.push_back(member->spec.key);
		keys.push_back(Fields::HelperEntity.key);
		keys.push_back(PluginMap::MassEntityManagerField.key);
		keys.push_back(kKeyGetSubsystem);
		keys.push_back(kKeyGetFragment);
		keys.push_back(kKeyIsValid);
		keys.push_back(kKeyIsBuilt);
		keys.insert(keys.end(), alsoNeeds.begin(), alsoNeeds.end());
		return PluginMap::DeclareFeatures(scanner, self, g_feature);
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		// Every key is asked even after a miss, so the log lists them all.
		bool all = true;
		for (SizeRow* row : kSizeRows)
			all = ResolveSize(scanner, self, *row) && all;

		for (Member* member : kMembers)
		{
			const uint32_t size = member->type ? member->type->size : member->fixed;
			if (!size)
				continue;   // its struct did not resolve; that miss is already recorded
			member->spec.size = size;
			all = PluginMap::ResolveField(scanner, self, member->spec) && all;
		}
		all = PluginMap::ResolveMassEntityManager(scanner, self) && all;

		const uintptr_t getSubsystem = ResolveFunction(scanner, self, kKeyGetSubsystem, kPatGetSubsystem, kAtGetSubsystem);
		const uintptr_t getFragment  = ResolveFunction(scanner, self, kKeyGetFragment, kPatGetFragment, kAtGetFragment);
		const uintptr_t isValid      = ResolveFunction(scanner, self, kKeyIsValid, kPatIsBuilt, kAtIsValid);
		const uintptr_t isBuilt      = ResolveFunction(scanner, self, kKeyIsBuilt, kPatIsBuilt, -1);
		g_getSubsystem = reinterpret_cast<GetSubsystemFn>(getSubsystem);
		g_getFragment  = reinterpret_cast<GetFragmentFn>(getFragment);
		g_isValid      = reinterpret_cast<EntityCheckFn>(isValid);
		g_isBuilt      = reinterpret_cast<EntityCheckFn>(isBuilt);

		g_resolved = all && getSubsystem && getFragment && isValid && isBuilt;
		if (!g_resolved || !g_feature.On())
			LOG_WARN("RailMass: rail sources are off on this build; the rest of WAILA is unaffected");
	}

	bool Available()
	{
		return g_resolved && g_feature.On() && !g_faulted
			&& g_getSubsystem && g_getFragment && g_isValid && g_isBuilt && Fields::HelperEntity.offset >= 0;
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
}
