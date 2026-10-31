// Console tests for Waila/rail_network.h: a few hand-built layouts, then the
// rails around a captured supermagnet platform (fixtures\supermagnet-platform.txt).
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
}

int main()
{
	std::string dir = __FILE__;
	dir.erase(dir.find_last_of("\\/") + 1);

	TestSocketNames();
	TestLanesStaySeparate();
	TestJunctionStorageAndInputs();
	TestLooseEnds();
	TestJunctionInsideDrill();
	TestLaneTracksStayApart();
	TestPlatform(dir);

	if (g_failures == 0)
		std::printf("All rail network tests passed.\n");
	return g_failures == 0 ? 0 : 1;
}
