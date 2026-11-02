// Console tests for Waila/rail_network.h: a few hand-built layouts, the rails
// around a captured supermagnet platform (fixtures\supermagnet-platform.txt),
// then the one-row-per-item sums of a rail card, its From/To lines and the
// meter that measures what crosses a rail.
// Prints each failure and returns non-zero when any check fails.

#include "rail_network.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Waila::RailNetwork;

namespace
{
	int g_failures = 0;

	void Check(bool ok, const std::string& what)
	{
		if (!ok)
		{
			++g_failures;
			std::printf("FAIL: %s\n", what.c_str());
		}
	}

	std::set<size_t> SourcesOf(const Scene& scene, size_t rail)
	{
		const Sources found = Graph(scene).Find(rail, 1024, 256);
		return { found.crafters.begin(), found.crafters.end() };
	}

	std::set<size_t> ConsumersOf(const Scene& scene, size_t rail)
	{
		const Graph graph(scene);
		const Consumers found = graph.FindConsumers(graph.Find(rail, 1024, 256), 1024, 256);
		return { found.crafters.begin(), found.crafters.end() };
	}

	size_t AddRail(Scene& scene, Vec3 start, Vec3 end)
	{
		scene.rails.push_back({ start, end });
		return scene.rails.size() - 1;
	}

	// A 2 m cube standing on the floor of its bounds.
	size_t AddBuilding(Scene& scene, Vec3 origin, bool crafter)
	{
		scene.buildings.push_back({ origin, { 100, 100, 100 }, origin.z - 100, crafter });
		return scene.buildings.size() - 1;
	}

	void TestSocketNames()
	{
		int32_t lane = 99;
		Check(SocketLane("RailSocket", lane) && lane == kAnyLane, "RailSocket joins every socket");
		Check(SocketLane("RailSocket3", lane) && lane == kAnyLane, "RailSocket3 joins every socket");
		Check(SocketLane("Rail_0B", lane) && lane == 0, "Rail_0B is lane 0");
		Check(SocketLane("rail_12a", lane) && lane == 12, "rail_12a is lane 12");
		Check(SocketLane("Rail_Socket", lane) && lane == kAnyLane, "Rail_Socket has no lane");
		Check(!SocketLane("PowerSocket", lane), "PowerSocket holds no rail");
	}

	// Two crafters feed two parallel tracks of one lane building. Each track's
	// far rail shows only its own crafter.
	void TestLanesStaySeparate()
	{
		Scene scene;
		const size_t a = AddBuilding(scene, { 0, 0, 0 }, true);
		const size_t b = AddBuilding(scene, { 0, 1000, 0 }, true);
		scene.AddPathPoint({ { "RailSocket", { 500, 0, 0 } },   { "Rail_0B", { 608, 0, 0 } },
		                     { "Rail_1A", { 500, 20, 0 } },     { "Rail_1B", { 608, 20, 0 } } });
		AddRail(scene, { 100, 0, 0 }, { 500, 0, 0 });
		AddRail(scene, { 0, 900, 0 }, { 500, 20, 0 });
		const size_t outA = AddRail(scene, { 608, 0, 0 }, { 2000, 0, 0 });
		const size_t outB = AddRail(scene, { 608, 20, 0 }, { 2000, 20, 0 });

		Check(SourcesOf(scene, outA) == std::set<size_t>{ a }, "lane 0 shows only its crafter");
		Check(SourcesOf(scene, outB) == std::set<size_t>{ b }, "lane 1 shows only its crafter");
	}

	// A junction joins all its sockets; a crafter's input rail does not show the
	// crafter it feeds; storage passes its inputs through.
	void TestJunctionStorageAndInputs()
	{
		Scene scene;
		const size_t a       = AddBuilding(scene, { 0, 0, 0 }, true);
		const size_t b       = AddBuilding(scene, { 0, 2000, 0 }, true);
		AddBuilding(scene, { 3000, 1000, 0 }, false);   // storage
		const size_t user    = AddBuilding(scene, { 6000, 1000, 0 }, true);
		scene.AddPathPoint({ { "RailSocket0", { 1500, 946, 0 } }, { "RailSocket1", { 1500, 1054, 0 } },
		                     { "RailSocket2", { 1554, 1000, 0 } } });
		AddRail(scene, { 100, 0, 0 }, { 1500, 946, 0 });
		AddRail(scene, { 0, 1900, 0 }, { 1500, 1054, 0 });
		const size_t toStorage = AddRail(scene, { 1554, 1000, 0 }, { 2900, 1000, 0 });
		const size_t toUser    = AddRail(scene, { 3100, 1000, 0 }, { 5900, 1000, 0 });
		const size_t fromUser  = AddRail(scene, { 6100, 1000, 0 }, { 9000, 1000, 0 });

		Check(SourcesOf(scene, toStorage) == std::set<size_t>{ a, b }, "junction joins both inputs");
		Check(SourcesOf(scene, toUser) == std::set<size_t>{ a, b }, "storage passes its inputs through");
		Check(SourcesOf(scene, fromUser) == std::set<size_t>{ user }, "an output rail shows only its own crafter");
	}

	// A crafter feeds a line; a branch off the first point ends at crafter m and
	// one into storage; a branch off the second point ends at crafter n; the line
	// ends at crafter z. A stretch shows the crafters branching off before it,
	// never storage and never the crafter its own end feeds.
	void TestConsumersBranchOffTheLine()
	{
		Scene scene;
		const size_t src     = AddBuilding(scene, { 0, 0, 0 }, true);
		const size_t m       = AddBuilding(scene, { 1500, 5100, 0 }, true);
		const size_t storage = AddBuilding(scene, { 1500, -5100, 0 }, false);
		const size_t n       = AddBuilding(scene, { 3000, 5100, 0 }, true);
		const size_t z       = AddBuilding(scene, { 5100, 0, 0 }, true);
		(void)src; (void)storage; (void)z;
		scene.AddPathPoint({ { "RailSocket0", { 1500, 0, 0 } }, { "RailSocket1", { 1554, 0, 0 } },
		                     { "RailSocket2", { 1500, 80, 0 } }, { "RailSocket3", { 1500, -80, 0 } } });
		scene.AddPathPoint({ { "RailSocket0", { 3000, 0, 0 } }, { "RailSocket1", { 3054, 0, 0 } },
		                     { "RailSocket2", { 3000, 80, 0 } } });
		const size_t first  = AddRail(scene, { 100, 0, 0 }, { 1500, 0, 0 });
		const size_t main   = AddRail(scene, { 1554, 0, 0 }, { 3000, 0, 0 });
		const size_t last   = AddRail(scene, { 3054, 0, 0 }, { 5000, 0, 0 });
		AddRail(scene, { 1500, 80, 0 }, { 1500, 5000, 0 });
		AddRail(scene, { 1500, -80, 0 }, { 1500, -5000, 0 });
		AddRail(scene, { 3000, 80, 0 }, { 3000, 5000, 0 });

		Check(ConsumersOf(scene, first).empty(), "the stretch out of the crafter has no branch before it");
		Check(ConsumersOf(scene, main) == std::set<size_t>{ m }, "after the first branch: crafter m, not the storage");
		Check(ConsumersOf(scene, last) == std::set<size_t>{ m, n }, "after the second: m and n, not the crafter at the end");
	}

	// Two rails from one point into the same crafter: neither counts the crafter
	// its own end feeds.
	void TestConsumersLeaveOutOwnEnd()
	{
		Scene scene;
		AddBuilding(scene, { 0, 0, 0 }, true);
		const size_t z = AddBuilding(scene, { 3000, 5100, 0 }, true);
		(void)z;
		scene.AddPathPoint({ { "RailSocket0", { 1500, 0, 0 } }, { "RailSocket1", { 1554, 0, 0 } },
		                     { "RailSocket2", { 1500, 80, 0 } } });
		AddRail(scene, { 100, 0, 0 }, { 1500, 0, 0 });
		const size_t a = AddRail(scene, { 1554, 0, 0 }, { 3000, 5000, 0 });
		const size_t b = AddRail(scene, { 1500, 80, 0 }, { 3000, 5000, 0 });

		Check(ConsumersOf(scene, a).empty(), "a rail does not count the crafter it feeds");
		Check(ConsumersOf(scene, b).empty(), "nor does the rail beside it into the same crafter");
	}

	// Ends of a path point the game has not spawned join within kJoinTolerance.
	void TestLooseEnds()
	{
		Scene scene;
		const size_t a = AddBuilding(scene, { 0, 0, 0 }, true);
		AddRail(scene, { 100, 0, 0 }, { 1000, 0, 0 });
		const size_t near = AddRail(scene, { 1110, 0, 0 }, { 3000, 0, 0 });
		const size_t far  = AddRail(scene, { 1000, 130, 0 }, { 3000, 500, 0 });

		Check(SourcesOf(scene, near) == std::set<size_t>{ a }, "loose ends 110 apart join");
		Check(SourcesOf(scene, far).empty(), "loose ends 130 apart do not");
	}

	// A junction the game has not spawned sits inside a drill's 20 m tall
	// bounds, fed by a smelter. The drill, the smelter and the rail ends are as
	// captured; the smelter's rail into the junction is one rail here.
	void TestJunctionInsideDrill()
	{
		Scene scene;
		scene.buildings.push_back({ { -226142.594, -54458.769, 3350.953 }, { 733.513, 445.099, 980.614 }, 3035.253, true });
		scene.buildings.push_back({ { -227473.150, -54400.280, 3659.540 }, { 304.462, 198.482, 269.806 }, 3390.0, true });
		const size_t drill = 0, smelter = 1;

		const size_t drillOut = AddRail(scene, { -226843.355, -54458.769, 3345.253 }, { -226814.850, -53800.000, 3553.930 });
		AddRail(scene, { -227760, -54400, 3650 }, { -226400, -54854, 4116 });
		const size_t outA = AddRail(scene, { -226346, -54800, 4116 }, { -224854, -54800, 4116 });
		const size_t outB = AddRail(scene, { -226454, -54800, 4116 }, { -226500, -55003.953, 4210.188 });

		Check(SourcesOf(scene, drillOut) == std::set<size_t>{ drill }, "the drill's output shows the drill");
		Check(SourcesOf(scene, outA) == std::set<size_t>{ smelter }, "a junction inside the drill's bounds passes the smelter on");
		Check(SourcesOf(scene, outB) == std::set<size_t>{ smelter }, "both of its outputs show the smelter");
	}

	// Three parallel tracks cross a lane building and only the middle one is
	// fed. The socket and rail end positions at the lane are from a captured
	// Lane_3; the crafter and the far ends of its rails are made up. Each track
	// keeps to itself, whether the lane has loaded or its rail ends are loose.
	Scene LaneScene(bool loaded, size_t& crafter, std::vector<size_t>& out)
	{
		Scene scene;
		crafter = AddBuilding(scene, { -223654, -53200, 4950 }, true);
		if (loaded)
		{
			scene.AddPathPoint({ { "RailSocket", { -223254, -53200, 6146 } }, { "Rail_0B", { -223146, -53200, 6146 } },
			                     { "Rail_1A",    { -223254, -53220, 6146 } }, { "Rail_1B", { -223146, -53220, 6146 } },
			                     { "Rail_2A",    { -223254, -53180, 6146 } }, { "Rail_2B", { -223146, -53180, 6146 } } });
		}
		AddRail(scene, { -223854, -53200, 5726 }, { -223254, -53180, 6146 });
		AddRail(scene, { -223654, -53200, 5050 }, { -223254, -53200, 6146 });
		AddRail(scene, { -223454, -53200, 5586 }, { -223254, -53220, 6146 });
		out = { AddRail(scene, { -223146, -53180, 6146 }, { -223146, -53180, 6566 }),
		        AddRail(scene, { -223146, -53200, 6146 }, { -223146, -53200, 6566 }),
		        AddRail(scene, { -223146, -53220, 6146 }, { -223146, -53220, 6566 }) };
		return scene;
	}

	void TestLaneTracksStayApart()
	{
		for (bool loaded : { true, false })
		{
			size_t              crafter = 0;
			std::vector<size_t> out;
			const Scene         scene = LaneScene(loaded, crafter, out);
			const std::string   what  = loaded ? "a loaded lane" : "a lane that has not loaded";

			Check(SourcesOf(scene, out[1]) == std::set<size_t>{ crafter }, what + " carries its fed track");
			Check(SourcesOf(scene, out[0]).empty() && SourcesOf(scene, out[2]).empty(), what + " keeps its other tracks apart");
		}
	}

	struct Fixture
	{
		Scene                              scene;
		std::unordered_map<long, size_t>   rail;       // fixture id -> scene index
		std::unordered_map<size_t, long>   buildingId; // scene index -> fixture id
		std::unordered_map<size_t, std::string> item;
	};

	bool Load(const std::string& path, Fixture& out)
	{
		std::ifstream in(path);
		if (!in)
			return false;

		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream s(line);
			std::string kind, cls, item;
			long id = 0;
			if (!(s >> kind) || kind[0] == '#' || !(s >> id))
				continue;

			if (kind == "rail")
			{
				Rail r;
				s >> r.start.x >> r.start.y >> r.start.z >> r.end.x >> r.end.y >> r.end.z;
				out.rail[id] = out.scene.rails.size();
				out.scene.rails.push_back(r);
			}
			else if (kind == "crafter" || kind == "storage")
			{
				Building b;
				s >> cls;
				if (kind == "crafter")
					s >> item;
				s >> b.origin.x >> b.origin.y >> b.origin.z >> b.extent.x >> b.extent.y >> b.extent.z >> b.floorZ;
				b.crafter = kind == "crafter";
				out.buildingId[out.scene.buildings.size()] = id;
				out.item[out.scene.buildings.size()] = item;
				out.scene.buildings.push_back(b);
			}
			else if (kind == "point")
			{
				s >> cls;
				std::vector<NamedSocket> sockets;
				NamedSocket socket;
				while (s >> socket.name >> socket.at.x >> socket.at.y >> socket.at.z)
					sockets.push_back(socket);
				out.scene.AddPathPoint(sockets);
			}
		}
		return true;
	}

	void TestPlatform(const std::string& dir)
	{
		Fixture f;
		if (!Load(dir + "fixtures\\supermagnet-platform.txt", f))
		{
			Check(false, "fixture loads");
			return;
		}

		const Graph graph(f.scene);
		auto ids = [&](size_t rail)
		{
			std::set<long> out;
			for (size_t c : graph.Find(rail, 1024, 256).crafters)
				out.insert(f.buildingId[c]);
			return out;
		};

		size_t outputs = 0, inputs = 0, largest = 0;
		for (size_t r = 0; r < f.scene.rails.size(); ++r)
		{
			const Sources found = graph.Find(r, 1024, 256);
			largest = std::max(largest, found.rails);

			const Port& start = graph.StartOf(r);
			if (start.kind == Port::Kind::Building && f.scene.buildings[start.index].crafter)
				++outputs;

			// A rail into a crafter never shows that crafter.
			const Vec3& end = f.scene.rails[r].end;
			for (size_t b = 0; b < f.scene.buildings.size(); ++b)
			{
				const Building& box = f.scene.buildings[b];
				if (!box.crafter
				 || std::abs(end.x - box.origin.x) > box.extent.x + kBuildingMargin
				 || std::abs(end.y - box.origin.y) > box.extent.y + kBuildingMargin
				 || std::abs(end.z - box.origin.z) > box.extent.z + kBuildingMargin)
					continue;
				++inputs;
				Check(std::find(found.crafters.begin(), found.crafters.end(), b) == found.crafters.end(),
				      "rail " + std::to_string(r) + " does not show the crafter it feeds");
			}
		}

		// The rails leaving a crafter, each carrying that crafter's output and
		// nothing else: six furnaces, two WolframPlate crafters and the drill.
		const std::pair<long, long> leaving[] = {
			{ 142428, 140907 }, { 140591, 140611 }, { 113035, 92073 }, { 301508, 301218 }, { 301553, 301533 },
			{ 302914, 302806 }, { 316068, 349173 }, { 316141, 349650 }, { 303440, 349745 } };
		for (const auto& [rail, crafter] : leaving)
			Check(ids(f.rail.at(rail)) == std::set<long>{ crafter }, "rail " + std::to_string(rail) + " shows only the crafter it leaves");
		Check(outputs == std::size(leaving), "the platform has " + std::to_string(std::size(leaving))
		      + " crafter output rails (found " + std::to_string(outputs) + ")");
		Check(inputs >= 10, "the platform has crafter input rails");
		Check(largest < 64, "no rail walks the whole platform (largest walk " + std::to_string(largest) + ")");

		// The merger into the platform's storage carries all six furnaces.
		Check(ids(f.rail.at(142511)) == std::set<long>{ 92073, 140611, 140907, 301218, 301533, 302806 },
		      "the rail into storage shows the six furnaces");

		// Out of storage, carrying what the six furnaces fill it with.
		Check(ids(f.rail.at(153565)) == std::set<long>{ 92073, 140611, 140907, 301218, 301533, 302806 },
		      "the rail out of storage shows the six furnaces");

		// Out of a merger the game has not spawned, joined by loose ends.
		Check(ids(f.rail.at(302060)) == std::set<long>{ 301218, 302806 },
		      "the unspawned merger's output shows its two furnaces");

		// Out of a junction the game has not spawned, inside the drill's bounds,
		// and on to the WolframPlate crafters. The smelter feeding it had not
		// loaded, so these show nothing rather than the drill.
		for (long rail : { 303344L, 303513L, 304120L, 314858L })
			Check(ids(f.rail.at(rail)).empty(), "rail " + std::to_string(rail) + " does not show the drill");
	}

	const ItemFlow* FlowOf(const std::vector<ItemFlow>& flows, const std::string& item)
	{
		for (const ItemFlow& f : flows)
			if (f.item == item)
				return &f;
		return nullptr;
	}

	bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

	FlowPart Maker(uint64_t building, const std::string& item, float full)
	{
		return { building, item, true, true, full };
	}

	FlowPart Taker(uint64_t building, const std::string& item, float full)
	{
		return { building, item, false, true, full };
	}

	// Three fabricators make wolfram plate, one seen both through the layout and
	// through the drones; a furnace makes chemicals. Five furnaces and a press
	// take plates, two of them seen twice; two furnaces take chemicals. One row
	// per item, each building counted once, the rates summed.
	void TestFlowsOneRowPerItem()
	{
		const std::vector<FlowPart> parts = {
			// The rail layout.
			Maker(1, "Plate", 30), Maker(2, "Plate", 30), Maker(3, "Plate", 30),
			Taker(10, "Plate", 10), Taker(11, "Plate", 10), Taker(12, "Plate", 10),
			Taker(10, "Acid", 10),   // the furnace's other input, not on this rail
			// The drones: fabricator 3 and furnaces 10 and 11 again, then more.
			Maker(3, "Plate", 30), Taker(10, "Plate", 10), Taker(11, "Plate", 10),
			Taker(13, "Plate", 10), Taker(14, "Plate", 10), Taker(15, "Plate", 20),
			Maker(4, "Chemicals", 20),
			Taker(10, "Chemicals", 5), Taker(16, "Chemicals", 5),
		};

		const std::vector<ItemFlow> flows = SumFlows(parts, 480.0f);
		Check(flows.size() == 2, "one row per carried item (got " + std::to_string(flows.size()) + ")");
		Check(!FlowOf(flows, "Acid"), "an item nothing on the rail supplies gets no row");

		const ItemFlow* plate = FlowOf(flows, "Plate");
		Check(plate && plate->sources == 3 && plate->consumers == 6, "plate: 3 sources, 6 consumers, duplicates counted once");
		Check(plate && Near(plate->max, 90), "plate max: 3 x 30");
		Check(plate && Near(plate->min, 20), "plate min: 90 - (5 x 10 + 20)");

		const ItemFlow* chem = FlowOf(flows, "Chemicals");
		Check(chem && chem->sources == 1 && chem->consumers == 2, "chemicals: 1 source, 2 consumers");
		Check(chem && Near(chem->max, 20) && Near(chem->min, 10), "chemicals: 20, 10");
		Check(flows.front().item == "Plate", "the biggest flow comes first");
	}

	// The rail caps the supply; nothing goes negative; an unread source leaves
	// its figures unknown; stock supplies at the rail's rate.
	void TestFlowsLimits()
	{
		std::vector<ItemFlow> flows = SumFlows({ Maker(1, "A", 600), Taker(2, "A", 900) }, 480.0f);
		Check(Near(flows[0].max, 480) && Near(flows[0].min, 0), "capped at the rail, floored at 0");

		flows = SumFlows({ Maker(1, "A", 60), Maker(2, "A", -1) }, 480.0f);
		Check(flows[0].max < 0 && flows[0].min < 0, "an unread source makes the row unknown");

		flows = SumFlows({ Maker(1, "A", 60) }, -1.0f);
		Check(flows[0].max < 0 && Near(flows[0].min, 60), "no capacity: no max, min from the sources");

		const FlowPart stock{ 7, "A", true, false };
		const FlowPart bin{ 8, "A", false, false };
		flows = SumFlows({ stock, bin, Taker(2, "A", 30) }, 120.0f);
		Check(flows[0].sources == 1 && flows[0].consumers == 2, "storage counts as a building at either end");
		Check(Near(flows[0].max, 120) && Near(flows[0].min, 90), "storage alone supplies at the rail's rate");

		flows = SumFlows({ stock, Maker(1, "A", 30) }, 120.0f);
		Check(Near(flows[0].max, 30), "with a crafter source, the crafters set the supply");
	}

	FlowPart Named(FlowPart part, const std::string& name, float now)
	{
		part.name = name;
		part.now  = now;
		return part;
	}

	// The From/To lines come from the same parts as the totals, so a line's
	// full rates add up to the row's output: buildings of a name merge, each
	// building once, sources first, and items not on the rail get no line.
	void TestEndLines()
	{
		const std::vector<FlowPart> parts = {
			Named(Taker(10, "Plate", 10), "Furnace", 0),
			Named(Maker(1, "Plate", 120), "Fabricator", 120),
			Named(Maker(2, "Plate", 120), "Fabricator", 0),
			Named(Maker(2, "Plate", 120), "Fabricator", 0),   // seen again by the drones
			Named(Maker(3, "Plate", 120), "Fabricator", -1),  // state unread
			Named(Taker(11, "Plate", 90), "Mega Press", 90),
			Named(Taker(10, "Acid", 10), "Furnace", 0),       // not on this rail
			Named(FlowPart{ 7, "Plate", true, false }, "Storage", -1),
		};
		const std::vector<EndRow> rows = GroupEnds(parts, { "Plate" });
		Check(rows.size() == 4, "fabricators, storage, furnace, press (got " + std::to_string(rows.size()) + ")");
		if (rows.size() != 4)
			return;
		Check(rows[0].source && rows[0].name == "Fabricator" && rows[0].count == 3, "3x Fabricator first, counted once each");
		Check(Near(rows[0].full, 360) && Near(rows[0].now, 120), "fabricators: 360 full, 120 now (unread left out)");
		Check(rows[1].source && rows[1].name == "Storage" && rows[1].full < 0 && rows[1].now < 0, "storage: no rate");
		Check(!rows[2].source && rows[2].name == "Furnace" && Near(rows[2].full, 10) && Near(rows[2].now, 0), "furnace: 10, stalled");
		Check(!rows[3].source && rows[3].name == "Mega Press" && Near(rows[3].full, 90), "press: 90");

		const std::vector<ItemFlow> flows = SumFlows(parts, 480.0f);
		Check(Near(FlowOf(flows, "Plate")->output, rows[0].full), "the row's output is the From lines' full rates");
	}

	// The meter counts a drone once per arrival on the lane, leaves out the ones
	// already there at the start, waits before giving a rate and forgets passes
	// past the window.
	void TestFlowMeter()
	{
		using Lane = std::vector<FlowMeter::OnLane>;
		FlowMeter meter;
		meter.Sample(0.0, { { 1, "Plate" } });   // already on the lane: not counted
		Check(!meter.Ready(0.0) && meter.PerMin(0.0, "Plate") < 0, "no rate before the window has run");

		// A drone every second for 30 s, each on the lane for two samples.
		double t = 0.0;
		for (int i = 1; i <= 30; ++i)
		{
			const uint64_t drone = 100 + static_cast<uint64_t>(i);
			t = i;
			meter.Sample(t - 0.5, Lane{ { drone, i % 3 == 0 ? "Acid" : "Plate" } });
			meter.Sample(t, Lane{ { drone, i % 3 == 0 ? "Acid" : "Plate" } });
		}
		Check(meter.Ready(t), "ready after 20 s");
		Check(Near(meter.PerMin(t, "Plate"), 20.0f / 30.0f * 60.0f), "plate: 20 in 30 s, 40 a minute");
		Check(Near(meter.PerMin(t, "Acid"), 10.0f / 30.0f * 60.0f), "acid: 10 in 30 s, 20 a minute");
		Check(meter.Items(t) == std::vector<std::string>{ "Plate", "Acid" }, "both items, in the order seen");

		// An empty lane for two minutes: every pass falls out of the window.
		meter.Sample(t + 120.0, {});
		Check(Near(meter.PerMin(t + 120.0, "Plate"), 0.0f) && meter.Items(t + 120.0).empty(), "the window forgets old passes");
	}
}

int main()
{
	std::string dir = __FILE__;
	dir.erase(dir.find_last_of("\\/") + 1);

	TestSocketNames();
	TestLanesStaySeparate();
	TestJunctionStorageAndInputs();
	TestLooseEnds();
	TestConsumersBranchOffTheLine();
	TestConsumersLeaveOutOwnEnd();
	TestJunctionInsideDrill();
	TestLaneTracksStayApart();
	TestPlatform(dir);
	TestFlowsOneRowPerItem();
	TestFlowsLimits();
	TestEndLines();
	TestFlowMeter();

	if (g_failures == 0)
		std::printf("All rail network tests passed.\n");
	return g_failures == 0 ? 0 : 1;
}
