#pragma once

// WAILA-RAIL-DIAG (whole file): a one-shot dump of the aimed rail's chain, for
// diagnosing the rail card from ModLoader.log. Removable: delete this file and
// every block between "WAILA-RAIL-DIAG BEGIN" and "WAILA-RAIL-DIAG END" (and the
// "Rail Diagnostic Dump" setting). Nothing here may go upstream.
//
// Pure C++ so rail_network.h (and its unit tests) can use it. Game thread only:
// every caller runs inside the tick. The lines are collected while the real
// walk runs, then written to the log in one go; there is no file I/O of its own.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Waila::RailDiag
{
	// A dump never passes ~1800 log lines: the body stops at kBodyMax and says so,
	// the summary and card lines have their own room.
	constexpr size_t kBodyMax = 1600;
	constexpr size_t kTailMax = 100;
	constexpr size_t kCardMax = 40;

	inline bool     g_enabled     = false;     // the "Rail Diagnostic Dump" setting
	inline bool     g_active      = false;     // a dump is being collected
	inline bool     g_cardPending = false;     // the dump is out; the card's own lines are next
	inline uint32_t g_seq         = 0;
	inline size_t   g_dropped     = 0;
	inline size_t   g_cardLines   = 0;
	inline void   (*g_emit)(const char* line) = nullptr;   // writes one log line

	inline std::vector<std::string> g_body;   // the walk, in order
	inline std::vector<std::string> g_tail;   // what was handed to the card

	inline bool Active() { return g_active; }

// The arguments are only evaluated while a dump is being collected.
#define RAIL_DIAG_LINE(...) do { if (::Waila::RailDiag::g_active) ::Waila::RailDiag::Line(__VA_ARGS__); } while (0)
#define RAIL_DIAG_TAIL(...) do { if (::Waila::RailDiag::g_active) ::Waila::RailDiag::Tail(__VA_ARGS__); } while (0)
#define RAIL_DIAG_CARD(...) do { if (::Waila::RailDiag::g_cardPending) ::Waila::RailDiag::CardLine(__VA_ARGS__); } while (0)

	// "index/serial" of a Mass entity handle (Index low half, SerialNumber high).
	inline std::string Id(uint64_t entity)
	{
		char buf[40];
		snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(entity & 0xFFFFFFFFu), static_cast<unsigned>(entity >> 32));
		return buf;
	}

	inline std::string Format(const char* fmt, va_list args)
	{
		char buf[480];
		vsnprintf(buf, sizeof(buf), fmt, args);
		buf[sizeof(buf) - 1] = '\0';
		return buf;
	}

	inline void Emit(const std::string& line)
	{
		if (!g_emit)
			return;
		char out[512];
		snprintf(out, sizeof(out), "WailaRailDiag[%u] %s", g_seq, line.c_str());
		g_emit(out);
	}

	inline void Begin()
	{
		g_active = true;
		++g_seq;
		g_dropped = 0;
		g_cardLines = 0;
		g_body.clear();
		g_tail.clear();
	}

	inline void Line(const char* fmt, ...)
	{
		if (!g_active)
			return;
		if (g_body.size() >= kBodyMax)
		{
			++g_dropped;
			return;
		}
		va_list args;
		va_start(args, fmt);
		g_body.push_back(Format(fmt, args));
		va_end(args);
	}

	inline void Tail(const char* fmt, ...)
	{
		if (!g_active || g_tail.size() >= kTailMax)
			return;
		va_list args;
		va_start(args, fmt);
		g_tail.push_back(Format(fmt, args));
		va_end(args);
	}

	// Writes the dump out and ends the collection.
	inline void Flush()
	{
		if (!g_active)
			return;
		Emit("---- begin ----");
		for (const std::string& line : g_body)
			Emit(line);
		if (g_dropped)
		{
			char buf[96];
			snprintf(buf, sizeof(buf), "TRUNCATED: %zu more line(s) dropped after the first %zu", g_dropped, kBodyMax);
			Emit(buf);
		}
		for (const std::string& line : g_tail)
			Emit(line);
		Emit("---- end of walk (the card's own lines follow) ----");
		g_active      = false;
		g_cardPending = true;
		g_body.clear();
		g_tail.clear();
	}

	// One line of the card the dump produced, from Card::FromRail. Only while a
	// dump's card is pending.
	inline bool CardPending() { return g_cardPending; }

	inline void CardLine(const char* fmt, ...)
	{
		if (!g_cardPending || g_cardLines >= kCardMax)
			return;
		++g_cardLines;
		va_list args;
		va_start(args, fmt);
		Emit(Format(fmt, args));
		va_end(args);
	}

	inline void CardDone()
	{
		if (!g_cardPending)
			return;
		Emit("---- card done ----");
		g_cardPending = false;
	}
}
