#include "crafter_detector.h"
#include "waila_functions.h"
#include "waila_fields.h"
#include "plugin_helpers.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"

#include <string>
#include <unordered_map>

using namespace SDK;

namespace Waila
{
	namespace
	{
		// A recipe stores ingredients as item *classes*; everything worth showing
		// (icon, display name, unique name) lives on the class default object.
		UAuItemDataBase* ItemDefaultsOf(UClass* itemClass)
		{
			if (!itemClass)
				return nullptr;

			UObject* cdo = itemClass->ClassDefaultObject;
			if (!cdo)
				return nullptr;

			bool isItem = false;
			try { isItem = cdo->IsA(UAuItemDataBase::StaticClass()); }
			catch (...) { return nullptr; }

			return isItem ? static_cast<UAuItemDataBase*>(cdo) : nullptr;
		}

		void FillEntry(RecipeItemEntry& entry, UClass* itemClass, int32_t count)
		{
			entry.itemClass = itemClass;
			entry.need      = count;
			entry.itemData  = ItemDefaultsOf(itemClass);

			if (entry.itemData)
				entry.displayName = Fields::DisplayNameOf(entry.itemData);
		}

		// Counts what is actually sitting in the crafter's input store, keyed by
		// the item's unique name so ingredients can be matched against it.
		// Returns false when the store could not be read, which is the difference
		// between "0 of these" and "we don't know".
		bool ReadInputStock(ACrBuildingActorBase* building,
		                    std::unordered_map<std::string, int32_t>& outStock)
		{
			UCrBuildingItemStorageComponent* inStorage = Fields::ValueOr<UCrBuildingItemStorageComponent*>(Fields::InItemStorage, building, nullptr);
			if (!inStorage)
				return false;

			auto fnGetContainer = Waila::Functions::GetStoredItemsContainerInternal();
			if (!fnGetContainer)
				return false;

			UWorld* world = PluginSdkRuntime::GetWorld();
			if (!world)
				return false;

			FCrItemsStorageContainer* container = fnGetContainer(inStorage, world);
			if (!container)
				return false;

			for (int i = 0; i < container->Items.Num(); ++i)
			{
				const FCrStorageItem& slot = container->Items[i];
				if (slot.bIsDisabled || slot.Item.Count <= 0)
					continue;
				if (!slot.Item.ItemDataBase || !UKismetSystemLibrary::IsValid(slot.Item.ItemDataBase))
					continue;

				outStock[Fields::UniqueNameOf(slot.Item.ItemDataBase)] += slot.Item.Count;
			}

			return true;
		}
	}

	bool CrafterDetector::IsCrafter(AActor* actor)
	{
		if (!actor || !UKismetSystemLibrary::IsValid(actor))
			return false;

		return actor->IsA(ACrCrafter::StaticClass());
	}

	bool CrafterDetector::GetCrafterInfo(AActor* actor, CrafterInfo& outInfo)
	{
		if (!IsCrafter(actor))
		{
			return false;
		}

		ACrCrafter* crafter = static_cast<ACrCrafter*>(actor);

		outInfo.crafterName = actor->GetName();

		// Resolve localized building name from PlacementData if available
		ACrBuildingActorBase* building = static_cast<ACrBuildingActorBase*>(actor);
		UCrBuildingData* buildingData = Fields::BuildingDataOf(building);

		outInfo.buildingData = buildingData;

		if (!Fields::ReadBuildingTexts(buildingData, outInfo.crafterClass, outInfo.buildingDesc))
		{
			outInfo.crafterClass = actor->Class ? actor->Class->GetName() : "Unknown";
			outInfo.buildingDesc = "N/A";
		}

		if (const FCrCraftingSettings* settings = Fields::CraftingSettings.In(crafter))
			outInfo.craftingSpeed = settings->CraftingSpeed;

		// Crafting progress (0.0 - 1.0)
		outInfo.craftingProgress = crafter->GetItemCraftingProgress();

		// Recipe — read the first queued item from the CraftComponent
		outInfo.currentRecipe = "(idle)";

		UCrCraftingComponent* comp = Fields::ValueOr<UCrCraftingComponent*>(Fields::CraftComponent, crafter, nullptr);

		if (comp)
		{
			const TArray<FAuCraftItem>* queue = Fields::ItemsToCraft.In(comp);
			const FAuCraftItem* first = queue && queue->Num() > 0 ? &(*queue)[0] : nullptr;

			if (first && first->RecipeData)
			{
				outInfo.recipeBuildTime = Fields::ValueOr<float>(Fields::RecipeBuildTime, first->RecipeData);
				const FAuItemRecipeOrder* recipeOutput = Fields::RecipeOutputItem.In(first->RecipeData);
				outInfo.recipeOutputCount = first->OutputItem.Count > 0 ? first->OutputItem.Count : (recipeOutput ? recipeOutput->Count : 0);
				UAuItemDataBase* itemData = first->RecipeData->GetItemDataBase();
				if (itemData)
				{
					outInfo.currentRecipe = Fields::UniqueNameOf(itemData);
					outInfo.currentRecipeDisplayName = Fields::DisplayNameOf(itemData);
				}

				// Store typed recipe pointer for the output icon (UCrItemRecipeData extends UAuItemRecipeData)
				if (first->RecipeData->IsA(UCrItemRecipeData::StaticClass()))
					outInfo.recipeDataPtr = static_cast<UCrItemRecipeData*>(first->RecipeData);

				// Recipe inputs and output. The have-counts come from the crafter's
				// own input store, so a starved machine can be spotted without
				// opening it.
				UAuItemRecipeData* recipe = first->RecipeData;

				std::unordered_map<std::string, int32_t> stock;
				const bool stockKnown = ReadInputStock(building, stock);

				const TArray<FAuItemRecipeOrder>* neededAt = Fields::NeededResources.In(recipe);
				const int32 neededCount = neededAt ? neededAt->Num() : 0;
				outInfo.recipeInputs.reserve(static_cast<size_t>(neededCount));

				for (int i = 0; i < neededCount; ++i)
				{
					const FAuItemRecipeOrder& order = (*neededAt)[i];
					RecipeItemEntry entry;
					FillEntry(entry, order.Item, order.Count);
					if (!entry.IsValid())
						continue;

					if (stockKnown && entry.itemData)
					{
						auto it = stock.find(Fields::UniqueNameOf(entry.itemData));
						entry.have = it != stock.end() ? it->second : 0;
					}

					outInfo.recipeInputs.push_back(std::move(entry));
				}

				FillEntry(outInfo.recipeOutput, recipeOutput ? recipeOutput->Item : nullptr, outInfo.recipeOutputCount);
			}

			// pattern: UCrCraftingComponent::GetCraftingFragment
			// 48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 99 ...
			auto fnGetFrag = Waila::Functions::GetCraftingFragment();
			if (fnGetFrag)
			{
				const FCrCraftingFragment* frag = fnGetFrag(comp);
				if (frag)
				{
					outInfo.craftingMultiplier = frag->CraftingMultiplier;
					outInfo.bMissingItems      = frag->bIsMissingItems;
					outInfo.bOutputFull        = frag->bOutputFull;
				}
			}
		}

		return true;
	}
}
