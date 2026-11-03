#include "card_model.h"
#include "icon_cache.h"
#include "ui_draw.h"

#include "crafter_detector.h"
#include "storage_detector.h"
#include "power_detector.h"
#include "cooler_active_detector.h"
#include "cooler_passive_detector.h"
#include "cargo_sender_detector.h"
#include "cargo_receiver_detector.h"
#include "rail_detector.h"
// WAILA-RAIL-DIAG BEGIN
#include "rail_diag.h"
// WAILA-RAIL-DIAG END

#include <cmath>
#include <cstdio>

namespace Waila::UI
{
	namespace
	{
		// More than this and the row stops being scannable, which is the whole
		// point of the layout.
		constexpr size_t kMaxStockSlots = 6;

		std::string Num(int32_t v)
		{
			char buf[24];
			snprintf(buf, sizeof(buf), "%d", v);
			return buf;
		}

		std::string Watts(float w)
		{
			char num[24];
			Draw::FormatNumber(num, sizeof(num), w);

			char buf[40];
			snprintf(buf, sizeof(buf), "%s W", num);
			return buf;
		}

		bool g_showDescriptions = true;

		void ApplyDescription(CardModel& out, const std::string& desc)
		{
			if (desc.empty() || desc == "N/A" || !g_showDescriptions)
				return;

			out.description = desc;
		}

		// Shared header for every card: name, icon, and the description line.
		void BeginCard(CardModel& out, Accent accent, const std::string& name,
		               const std::string& desc, SDK::UCrBuildingData* buildingData)
		{
			out.Clear();
			out.valid        = true;
			out.accent       = accent;
			out.title        = name;
			out.buildingIcon = Icons::ForBuilding(buildingData);
			ApplyDescription(out, desc);
		}

		// Stack counts ride the corner of a 36pt tile, so they have to stay short —
		// a raw "12480" would be wider than the icon it belongs to.
		std::string NumCompact(int32_t v)
		{
			char buf[24];
			if (v >= 100000)     snprintf(buf, sizeof(buf), "%dk", v / 1000);
			else if (v >= 10000) snprintf(buf, sizeof(buf), "%.0fk", v / 1000.0f);
			else if (v >= 1000)  snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0f);
			else                 snprintf(buf, sizeof(buf), "%d", v);
			return buf;
		}

		FlowSlot StockSlot(PluginTextureHandle icon, int32_t count)
		{
			FlowSlot slot;
			slot.icon  = icon;
			slot.label = NumCompact(count);
			return slot;
		}
	}

	void Card::SetShowDescriptions(bool show)
	{
		g_showDescriptions = show;
	}

	// -------------------------------------------------------------------------
	// Crafter — the flow body this layout was designed around
	// -------------------------------------------------------------------------

	void Card::FromCrafter(const Waila::CrafterInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Crafter, info.crafterClass, info.buildingDesc, info.buildingData);

		const bool idle = info.currentRecipe == "(idle)" || info.recipeInputs.empty();

		const std::string recipeName = info.currentRecipeDisplayName.empty()
			? info.currentRecipe
			: info.currentRecipeDisplayName;

		out.progress      = info.craftingProgress;
		out.progressValid = !idle;

		// Speed only earns a chip when it is not the plain 1x every building has.
		if (std::fabs(info.craftingSpeed - 1.0f) > 0.005f)
		{
			char buf[24];
			char num[24];
			Draw::FormatNumber(num, sizeof(num), info.craftingSpeed);
			snprintf(buf, sizeof(buf), "%sx", num);
			out.chip = buf;
		}

		if (idle)
		{
			out.status     = Health::Idle;
			out.statusText = "Idle";
			out.footLeft   = "No recipe set";
			return;
		}

		// A stop the game reports for another reason comes first; then output full,
		// the more actionable of the two item stalls.
		if (info.bNoPower)
		{
			out.status     = Health::Bad;
			out.statusText = "No power";
		}
		else if (info.bOutputFull)
		{
			out.status     = Health::Bad;
			out.statusText = "Output full";
		}
		else if (info.bMissingItems)
		{
			out.status     = Health::Bad;
			out.statusText = "Starved";
		}
		else
		{
			out.status     = Health::Good;
			out.statusText = "Working";
		}

		out.hasFlow = true;

		for (const auto& in : info.recipeInputs)
		{
			FlowSlot slot;
			slot.icon = in.itemData ? Icons::ForItem(in.itemData) : Icons::ForItemClass(in.itemClass);

			// The badge states what the recipe wants a cycle.
			char buf[32];
			snprintf(buf, sizeof(buf), "%d", in.need);
			slot.label = buf;

			if (in.perMin >= 0.0f)
			{
				// What the machine needs a minute at full speed, above the tile.
				char full[24];
				Draw::FormatNumber(full, sizeof(full), in.perMin);
				slot.rate = std::string(full) + "/min";

				// Short when the store can't fund the next cycle and what has been
				// arriving doesn't make up the difference. A machine stopped for power
				// or blocked by a full output isn't waiting on its inputs, so none is blamed. Gaps under a tenth of
				// the need are delivery jitter, not a shortage.
				if (!info.bNoPower && !info.bOutputFull && in.have >= 0 && in.have < in.need && in.arrivalPerMin >= 0.0f)
				{
					const float missing = in.perMin - in.arrivalPerMin;
					if (missing >= in.perMin * 0.1f && missing >= 0.5f)
					{
						slot.shortage = "short " + Num(static_cast<int32_t>(std::lround(missing))) + "/min";
						slot.dim      = true;
						slot.tone     = Health::Bad;
					}
				}
			}

			out.inputs.push_back(std::move(slot));
		}

		if (info.recipeOutput.IsValid())
		{
			out.hasOutput = true;
			out.output.icon = Icons::ForRecipe(info.recipeDataPtr);
			if (!out.output.icon)
			{
				out.output.icon = info.recipeOutput.itemData
					? Icons::ForItem(info.recipeOutput.itemData)
					: Icons::ForItemClass(info.recipeOutput.itemClass);
			}

			// Always badged, even at 1 — a bare product tile in a row of counted
			// ingredients reads as an icon whose number failed to load.
			char buf[24];
			snprintf(buf, sizeof(buf), "%d", info.recipeOutputCount > 0 ? info.recipeOutputCount : 1);
			out.output.label = buf;
		}

		if (info.recipeBuildTime > 0.0f)
		{
			const float perMinute = info.outputPerMin > 0.0f ? info.outputPerMin : 0.0f;

			char num[24];
			Draw::FormatNumber(num, sizeof(num), out.status == Health::Bad ? 0.0f : perMinute);
			out.rateValue = num;
			out.rateUnit  = "/min";

			char cycle[56];
			char full[24];
			char secs[24];
			Draw::FormatNumber(full, sizeof(full), perMinute);
			Draw::FormatSeconds(secs, sizeof(secs), info.recipeBuildTime);
			snprintf(cycle, sizeof(cycle), "of %s/min, %s cycle", full, secs);
			out.rateSub = cycle;

			if (out.status != Health::Bad)
			{
				// The countdown takes the header slot instead of "Working" — a
				// pulsing green dot beside a ticking timer already says the machine
				// is running, and the word was costing a footer row to repeat it.
				// The stalled states keep their word; they have no countdown anyway.
				char next[40];
				char remain[24];
				const float remaining = info.recipeBuildTime * (1.0f - info.craftingProgress);
				Draw::FormatSeconds(remain, sizeof(remain), remaining < 0.0f ? 0.0f : remaining);
				snprintf(next, sizeof(next), "next in %s", remain);
				out.statusText = next;
			}
		}

		out.outputName = recipeName;
	}

	// -------------------------------------------------------------------------
	// Containers
	// -------------------------------------------------------------------------

	namespace
	{
		void FillContainer(CardModel& out, int32_t maxCapacity, int32_t usedSlots,
		                   const std::vector<Waila::StoredItemEntry>& items)
		{
			for (size_t i = 0; i < items.size() && i < kMaxStockSlots; ++i)
				out.stock.push_back(StockSlot(Icons::ForItem(items[i].itemData), items[i].count));

			// Say how many kinds got cut rather than silently showing a partial list.
			if (items.size() > kMaxStockSlots)
				out.stockMore = static_cast<int32_t>(items.size() - kMaxStockSlots);

			// Each stack carries its own count on its icon, so a total would only be
			// those numbers added up in front of the player.
			char stacks[32];
			snprintf(stacks, sizeof(stacks), "%d/%d", usedSlots, maxCapacity);
			out.stats.push_back({ stacks, "slots", Health::Idle });

			if (maxCapacity > 0)
			{
				out.progress      = static_cast<float>(usedSlots) / static_cast<float>(maxCapacity);
				out.progressValid = true;

				if (out.progress >= 0.999f)
				{
					out.status     = Health::Warn;
					out.statusText = "Full";
				}
				else
				{
					// The slot counts are already a stat cell — the chip says how
					// full rather than repeating them.
					char pct[16];
					snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(out.progress * 100.0f + 0.5f));
					out.status     = Health::Good;
					out.statusText = pct;
				}
			}
		}
	}

	void Card::FromStorage(const Waila::StorageInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Storage, info.buildingName, info.buildingDesc, info.buildingData);
		FillContainer(out, info.maxCapacity, info.usedSlots, info.storedItems);

		if (info.storedItems.empty())
		{
			out.status     = Health::Idle;
			out.statusText = "Empty";
		}
	}

	void Card::FromCargoReceiver(const Waila::CargoReceiverInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Cargo, info.buildingName, info.buildingDesc, info.buildingData);
		FillContainer(out, info.maxCapacity, info.usedSlots, info.storedItems);
		out.footLeft = "Receiving";

		if (info.storedItems.empty())
		{
			out.status     = Health::Idle;
			out.statusText = "Empty";
		}
	}

	// -------------------------------------------------------------------------
	// Drone rail
	// -------------------------------------------------------------------------

	namespace
	{
		// Past this the list stops being a glance: item rows and From/To rows
		// together, like the stock slots on a container.
		constexpr size_t kMaxRateRows = 6;

		std::string PerMin(float v)
		{
			if (v < 0.0f)
				return "?";

			// FormatNumber would round a slow trickle down to "0".
			if (v > 0.0f && v < 0.05f)
				return "<0.1";

			char buf[24];
			Draw::FormatNumber(buf, sizeof(buf), v);
			return buf;
		}
	}

	namespace
	{
		// What the storage on the rail's network holds of an item: "4.0k in
		// storage", or "12k in 3 storages".
		std::string StockText(const Waila::RailRateEntry& r)
		{
			char count[24];
			Draw::FormatNumber(count, sizeof(count), static_cast<float>(r.stored));
			return std::string(count) + (r.storages > 1 ? " in " + std::to_string(r.storages) + " storages" : " in storage");
		}

		// The machines of one name making an item, or taking it, under its row:
		// what they make or take running full out under max and right now under
		// now, summed over the line: a maker 0 while stalled (red), a consumer
		// 0 while it is not asking for the item. min is the rail's, so it stays
		// blank.
		RateRow PeerRow(const Waila::RailPeerEntry& p)
		{
			RateRow row;
			row.icon = Icons::ForItem(p.itemData);
			row.name = std::string(p.source ? "From " : "To ")
				+ (p.count > 1 ? std::to_string(p.count) + "x " : std::string()) + p.buildingName;
			if (p.perMinFull >= 0.0f)
				row.max = PerMin(p.perMinFull);
			if (p.perMinNow >= 0.0f)
			{
				row.now = PerMin(p.perMinNow);
				if (p.source && p.perMinNow <= 0.0f)
					row.tone = Health::Bad;
			}
			return row;
		}
	}

	void Card::FromRail(const Waila::RailInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Cargo, info.buildingName, info.buildingDesc, info.buildingData);

		out.hasRates = true;
		out.chip     = info.filterName;

		// Each item's rows together: its rates, the machines making it and
		// taking it, and its stock. An item only the storage holds is one row.
		// When they do not all fit, every item keeps its own row and the
		// machine and stock rows fill the room left, first item first.
		std::vector<std::vector<RateRow>> items;
		size_t total = 0;
		for (const Waila::RailRateEntry& r : info.rates)
		{
			std::vector<RateRow>& rows = items.emplace_back();
			RateRow row;
			row.icon = Icons::ForItem(r.itemData);
			if (!r.made)
			{
				row.name = r.displayName + ": " + StockText(r);
				rows.push_back(std::move(row));
				++total;
				continue;
			}
			row.name = r.displayName;
			row.min  = PerMin(r.perMinMin);
			row.now  = PerMin(r.perMinNow);
			row.max  = PerMin(r.perMinMax);
			rows.push_back(std::move(row));
			for (const Waila::RailPeerEntry& p : r.peers)
				rows.push_back(PeerRow(p));
			if (r.stored >= 0 && r.storages > 0)
			{
				RateRow stock;
				stock.icon = Icons::ForItem(r.itemData);
				stock.name = StockText(r);
				rows.push_back(std::move(stock));
			}
			total += rows.size();
		}
		size_t room  = kMaxRateRows > items.size() ? kMaxRateRows - items.size() : 0;
		size_t shown = 0;
		for (std::vector<RateRow>& rows : items)
		{
			if (shown >= kMaxRateRows)
				break;
			out.rates.push_back(std::move(rows[0]));
			++shown;
			for (size_t i = 1; i < rows.size() && room > 0; ++i, --room, ++shown)
				out.rates.push_back(std::move(rows[i]));
		}
		const size_t more = total - shown;

		if (out.rates.empty())
			out.ratesNote = info.fromMass && !info.note.empty() ? info.note : "No sources found";

		char buf[48];
		if (info.fromCount + info.destCount > 0)
		{
			snprintf(buf, sizeof(buf), "%d from, %d to%s", info.fromCount, info.destCount, info.truncated ? "+" : "");
			out.statusText = buf;
		}
		else if (info.sourceCount > 0)
		{
			snprintf(buf, sizeof(buf), "%d source%s%s", info.sourceCount,
				info.sourceCount == 1 ? "" : "s", info.truncated ? "+" : "");
			out.statusText = buf;
		}

		out.footLeft = "items / min: min, now, max";
		if (more > 0)
		{
			snprintf(buf, sizeof(buf), "+%d more", static_cast<int>(more));
			out.footRight = buf;
		}

		// WAILA-RAIL-DIAG BEGIN
		if (RailDiag::CardPending())
		{
			RAIL_DIAG_CARD("card: header \"%s\", %zu row(s) shown, footer-right \"%s\", note \"%s\" (%s)", out.statusText.c_str(),
				out.rates.size(), out.footRight.c_str(), out.ratesNote.c_str(),
				!out.rates.empty() ? "rows shown, no note"
				: info.fromMass && !info.note.empty() ? "note from RailDetector::GetRailInfo"
				: "fallback in Card::FromRail: not read from the Mass data, or no note");
			for (const RateRow& row : out.rates)
				RAIL_DIAG_CARD("  card row \"%s\" min=%s now=%s max=%s%s", row.name.c_str(), row.min.c_str(), row.now.c_str(),
					row.max.c_str(), row.tone == Health::Bad ? " (red)" : "");
			RailDiag::CardDone();
		}
		// WAILA-RAIL-DIAG END
	}

	// -------------------------------------------------------------------------
	// Power
	// -------------------------------------------------------------------------

	void Card::FromPower(const Waila::PowerInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Power, info.buildingName, info.buildingDesc, info.buildingData);

		switch (info.gridConnectionStatus)
		{
		case 1:
			out.status     = Health::Good;
			out.statusText = "On grid";
			break;
		case 2:
			out.status     = Health::Warn;
			out.statusText = "Switched off";
			break;
		default:
			out.status     = Health::Bad;
			out.statusText = "Off grid";
			break;
		}

		// The ring reads as grid load: how much of what is being generated is
		// already spoken for.
		if (info.gridAddPower > 0.0f)
		{
			out.progress      = info.gridRemovePower / info.gridAddPower;
			out.progressValid = true;
		}

		const bool overdrawn = info.gridRemovePower > info.gridAddPower;

		out.stats.push_back({ Watts(info.buildingPower),   "output",   Health::Idle });
		out.stats.push_back({ Watts(info.gridAddPower),    "grid gen", Health::Idle });
		out.stats.push_back({ Watts(info.gridRemovePower), "grid use", overdrawn ? Health::Bad : Health::Idle });
		out.stats.push_back({ Watts(info.gridTotalPower),  "grid cap", Health::Idle });

		if (overdrawn)
		{
			out.status     = Health::Bad;
			out.statusText = "Overdrawn";
		}
	}

	// -------------------------------------------------------------------------
	// Coolers
	// -------------------------------------------------------------------------

	namespace
	{
		void FillCooler(CardModel& out, uint8_t state, int32_t connected, int32_t total)
		{
			const char* stateStr = "Unknown";
			Health      tone     = Health::Idle;

			switch (state)
			{
			case 0: stateStr = "Idle";      tone = Health::Idle; break;
			case 1: stateStr = "Working";   tone = Health::Good; break;
			case 2: stateStr = "No fuel";   tone = Health::Bad;  break;
			case 3: stateStr = "Too hot";   tone = Health::Bad;  break;
			default: break;
			}

			out.status     = tone;
			out.statusText = stateStr;

			if (total > 0)
			{
				out.progress      = static_cast<float>(connected) / static_cast<float>(total);
				out.progressValid = true;
			}

			char sockets[32];
			snprintf(sockets, sizeof(sockets), "%d/%d", connected, total);
			out.stats.push_back({ sockets,  "sockets", connected == 0 ? Health::Warn : Health::Idle });
			out.stats.push_back({ stateStr, "state",   tone });
		}
	}

	void Card::FromCoolerActive(const Waila::CoolerActiveInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Cooler, info.buildingName, info.buildingDesc, info.buildingData);
		FillCooler(out, info.state, info.connectedSockets, info.totalSockets);
	}

	void Card::FromCoolerPassive(const Waila::CoolerPassiveInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Cooler, info.buildingName, info.buildingDesc, info.buildingData);
		FillCooler(out, info.state, info.connectedSockets, info.totalSockets);
	}

	// -------------------------------------------------------------------------
	// Cargo sender — the same equation, with a drone instead of a product
	// -------------------------------------------------------------------------

	void Card::FromCargoSender(const Waila::CargoSenderInfo& info, CardModel& out)
	{
		BeginCard(out, Accent::Cargo, info.buildingName, info.buildingDesc, info.buildingData);

		// canSend means the pad is free, i.e. it is loading rather than in flight.
		out.status     = info.canSend ? Health::Warn : Health::Good;
		out.statusText = info.canSend ? "Loading" : "Sending";

		const float progress = info.sendProgress < 0.0f ? 0.0f : info.sendProgress;
		out.progress      = progress;
		out.progressValid = info.sendProgress >= 0.0f;

		out.hasFlow = true;

		if (info.sendingItem)
		{
			FlowSlot slot;
			slot.icon = Icons::ForItem(info.sendingItem);
			out.inputs.push_back(std::move(slot));
		}

		char pct[16];
		snprintf(pct, sizeof(pct), "%d", static_cast<int>(progress * 100.0f + 0.5f));
		out.rateValue = pct;
		out.rateUnit  = "%";
		out.rateSub   = info.sendProgress >= 0.0f ? "loaded" : "idle";

		if (!info.sendingItemName.empty())
			out.outputName = info.sendingItemName;

		if (info.sendingTime > 0.0f)
		{
			char every[40];
			char secs[24];
			Draw::FormatSeconds(secs, sizeof(secs), info.sendingTime);
			snprintf(every, sizeof(every), "every %s", secs);
			out.footRight = every;
		}
	}
}
