#pragma once

// The drone rail topology, in plain C++ with no game types, so the same code
// runs in the plugin and in Tests\RailNetworkTests.

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Waila::RailNetwork
{
	struct Vec3
	{
		double x = 0.0, y = 0.0, z = 0.0;
	};

	// The world positions of a rail's first and last spline point. Drones fly a
	// rail from its start to its end. INFERRED from two captured saves: every
	// crafter's rails start on one face of it (its output) and end on another
	// (its inputs), and every input of a merger is a rail end.
	struct Rail
	{
		Vec3 start;
		Vec3 end;
	};

	// One socket of a path point, as the game names it.
	struct NamedSocket
	{
		std::string name;
		Vec3        at;
	};

	// A rail socket of a path point (a pole, rail support, junction, merger or
	// lane). Sockets of one point with the same lane are connected.
	struct Socket
	{
		size_t  point;
		int32_t lane;
		Vec3    at;
	};

	// A building drones bring items to or take them from: a crafter, storage, a
	// drone station. `origin` and `extent` are its actor bounds, `floorZ` the
	// height of its actor location, which is where it stands.
	struct Building
	{
		Vec3   origin;
		Vec3   extent;
		double floorZ  = 0.0;
		bool   crafter = false;
	};

	constexpr int32_t kAnyLane = -1;

	// The lane of a rail socket, from its name. A lane building carries parallel
	// tracks that must not mix: its sockets are "Rail_<n>A" and "Rail_<n>B", and
	// its plain "RailSocket" is track 0. Every other path point joins all its
	// sockets (kAnyLane). Returns false for a socket that holds no rail.
	inline bool SocketLane(std::string_view name, int32_t& outLane)
	{
		auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
		auto startsWith = [&](std::string_view prefix)
		{
			if (name.size() < prefix.size())
				return false;
			for (size_t i = 0; i < prefix.size(); ++i)
			{
				if (lower(name[i]) != prefix[i])
					return false;
			}
			return true;
		};

		if (!startsWith("rail"))
			return false;

		outLane = kAnyLane;
		if (startsWith("rail_") && name.size() >= 7)
		{
			int32_t lane = 0;
			size_t  i    = 5;
			for (; i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); ++i)
				lane = lane * 10 + (name[i] - '0');

			const char side = i < name.size() ? lower(name[i]) : '\0';
			if (i > 5 && i + 1 == name.size() && (side == 'a' || side == 'b'))
				outLane = lane;
		}
		return true;
	}

	struct Scene
	{
		std::vector<Rail>     rails;
		std::vector<Socket>   sockets;
		std::vector<Building> buildings;
		size_t                points = 0;

		// Adds one path point with all its sockets. Sockets that hold no rail are
		// skipped.
		void AddPathPoint(const std::vector<NamedSocket>& named)
		{
			const size_t first = sockets.size();
			bool hasLanes = false;
			for (const NamedSocket& s : named)
			{
				int32_t lane = kAnyLane;
				if (!SocketLane(s.name, lane))
					continue;
				hasLanes = hasLanes || lane != kAnyLane;
				sockets.push_back({ points, lane, s.at });
			}
			if (hasLanes)
			{
				for (size_t i = first; i < sockets.size(); ++i)
				{
					if (sockets[i].lane == kAnyLane)
						sockets[i].lane = 0;
				}
			}
			++points;
		}
	};

	struct Sources
	{
		std::vector<size_t> crafters;    // indices into Scene::buildings
		size_t              rails = 0;   // how many rails the walk reached, the start one included
		bool                truncated = false;
	};

	// INFERRED, as is everything about which buildings a rail serves: the SDK
	// has no explicit building-to-rail link. ConnectedStartActor, ConnectedEndActor
	// and SocketActor were null on every rail of two captured saves, so the
	// topology comes from where each rail's spline ends sit.
	//
	// A rail end on a path point's rail socket belongs to that point: in both
	// captures every such end lay within 0.5 units of its socket, and each socket
	// held one rail. Supply buildings expose no rail sockets (their SocketsData
	// held none), so an end belongs to a building when it lies within
	// kBuildingMargin of its bounds at port height. In four captures every such
	// end sat 260 to 332 units above the building's floor, on furnaces,
	// crafters, smelters, a drill, storage and a turret. A drill's bounds are
	// 20 m tall, and the ends of a junction that had not loaded sat inside them
	// 1081 above the drill's floor.
	//
	// Path points the game has not spawned as actors leave their rail ends loose.
	// A loose rail start is fed by the loose rail ends within kJoinTolerance: the
	// sockets of a pole sit 30 units apart, those of a junction 76 to 108, and a
	// merger's outer input 117 from its output. The next gap measured was 130.
	// The tracks of a lane building that has not loaded join each other this way.
	constexpr double kSocketTolerance = 5.0;     // unreal units, 100 = 1 m
	constexpr double kBuildingMargin  = 50.0;
	constexpr double kPortHeight      = 600.0;
	constexpr double kJoinTolerance   = 120.0;

	namespace Detail
	{
		inline double DistSq(const Vec3& a, const Vec3& b)
		{
			const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
			return x * x + y * y + z * z;
		}

		// Points bucketed in cubes of `cell` units, so the points near a position
		// are found by checking the 27 cubes around it. Queries reach at most one
		// cell out.
		class PointGrid
		{
		public:
			explicit PointGrid(double cell) : m_cell(cell) {}

			void Add(size_t id, const Vec3& at) { m_cells[CellOf(at)].push_back({ id, at }); }

			template <typename Fn>
			void ForNear(const Vec3& at, Fn&& fn) const
			{
				const Cell c = CellOf(at);
				for (int64_t dx = -1; dx <= 1; ++dx)
				for (int64_t dy = -1; dy <= 1; ++dy)
				for (int64_t dz = -1; dz <= 1; ++dz)
				{
					const auto it = m_cells.find({ c.x + dx, c.y + dy, c.z + dz });
					if (it == m_cells.end())
						continue;
					for (const Entry& e : it->second)
						fn(e.id, e.at);
				}
			}

		private:
			struct Cell
			{
				int64_t x, y, z;
				bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
			};

			struct CellHash
			{
				size_t operator()(const Cell& c) const
				{
					return static_cast<size_t>(c.x * 73856093LL ^ c.y * 19349663LL ^ c.z * 83492791LL);
				}
			};

			struct Entry
			{
				size_t id;
				Vec3   at;
			};

			Cell CellOf(const Vec3& p) const
			{
				return { static_cast<int64_t>(std::floor(p.x / m_cell)),
				         static_cast<int64_t>(std::floor(p.y / m_cell)),
				         static_cast<int64_t>(std::floor(p.z / m_cell)) };
			}

			double                                           m_cell;
			std::unordered_map<Cell, std::vector<Entry>, CellHash> m_cells;
		};
	}

	// What a rail end sits on.
	struct Port
	{
		enum class Kind : uint8_t { Loose, Socket, Building };

		Kind   kind  = Kind::Loose;
		size_t index = 0;   // into Scene::sockets or Scene::buildings
	};

	class Graph
	{
	public:
		explicit Graph(const Scene& scene)
			: m_scene(scene), m_looseEnds(kJoinTolerance)
		{
			Detail::PointGrid sockets(kJoinTolerance);
			for (size_t i = 0; i < scene.sockets.size(); ++i)
				sockets.Add(i, scene.sockets[i].at);

			m_start.resize(scene.rails.size());
			m_endsAtBuilding.resize(scene.buildings.size());

			for (size_t r = 0; r < scene.rails.size(); ++r)
			{
				m_start[r] = Place(scene.rails[r].start, sockets);

				const Port end = Place(scene.rails[r].end, sockets);
				switch (end.kind)
				{
				case Port::Kind::Socket:
				{
					const Socket& s = scene.sockets[end.index];
					m_endsAtPoint[{ s.point, s.lane }].push_back(r);
					break;
				}
				case Port::Kind::Building:
					m_endsAtBuilding[end.index].push_back(r);
					break;
				case Port::Kind::Loose:
					m_looseEnds.Add(r, scene.rails[r].end);
					break;
				}
			}
		}

		const Port& StartOf(size_t rail) const { return m_start[rail]; }

		// The crafters whose output reaches `rail`, walking against the flow: from
		// the rail's start to the rails that end there, and on. A crafter at a
		// rail's start is a source and the walk stops there, so a factory's input
		// rails never show the factory they feed. Other supply buildings, such as
		// storage, pass their input rails through to their output rails.
		Sources Find(size_t rail, size_t maxRails, size_t maxSources) const
		{
			Sources out;
			if (rail >= m_scene.rails.size())
				return out;

			std::vector<bool>          seen(m_scene.rails.size(), false);
			std::vector<size_t>        queue{ rail };
			std::unordered_set<size_t> found;
			seen[rail] = true;

			auto visit = [&](size_t next)
			{
				if (seen[next])
					return true;
				if (queue.size() >= maxRails)
				{
					out.truncated = true;
					return false;
				}
				seen[next] = true;
				queue.push_back(next);
				return true;
			};

			for (size_t head = 0; head < queue.size() && !out.truncated; ++head)
			{
				const size_t current = queue[head];
				const Port&  start   = m_start[current];

				switch (start.kind)
				{
				case Port::Kind::Socket:
				{
					const Socket& s = m_scene.sockets[start.index];
					const auto it = m_endsAtPoint.find({ s.point, s.lane });
					if (it == m_endsAtPoint.end())
						break;
					for (size_t next : it->second)
					{
						if (!visit(next))
							break;
					}
					break;
				}
				case Port::Kind::Building:
				{
					if (m_scene.buildings[start.index].crafter)
					{
						if (found.count(start.index))
							break;
						if (out.crafters.size() >= maxSources)
						{
							out.truncated = true;
							break;
						}
						found.insert(start.index);
						out.crafters.push_back(start.index);
						break;
					}
					for (size_t next : m_endsAtBuilding[start.index])
					{
						if (!visit(next))
							break;
					}
					break;
				}
				case Port::Kind::Loose:
				{
					const Vec3&  at    = m_scene.rails[current].start;
					const double limit = kJoinTolerance * kJoinTolerance;
					bool         more  = true;
					m_looseEnds.ForNear(at, [&](size_t next, const Vec3& end)
					{
						if (more && Detail::DistSq(at, end) <= limit)
							more = visit(next);
					});
					break;
				}
				}
			}

			out.rails = queue.size();
			return out;
		}

	private:
		Port Place(const Vec3& at, const Detail::PointGrid& sockets) const
		{
			Port   port;
			double best = kSocketTolerance * kSocketTolerance;
			sockets.ForNear(at, [&](size_t i, const Vec3& socket)
			{
				const double sq = Detail::DistSq(at, socket);
				if (sq <= best)
				{
					best       = sq;
					port.kind  = Port::Kind::Socket;
					port.index = i;
				}
			});
			if (port.kind == Port::Kind::Socket)
				return port;

			// The building whose grown bounds hold the end at port height, the
			// nearest centre winning when bounds overlap.
			for (size_t b = 0; b < m_scene.buildings.size(); ++b)
			{
				const Building& box = m_scene.buildings[b];
				if (std::abs(at.x - box.origin.x) > box.extent.x + kBuildingMargin
				 || std::abs(at.y - box.origin.y) > box.extent.y + kBuildingMargin
				 || std::abs(at.z - box.origin.z) > box.extent.z + kBuildingMargin
				 || at.z - box.floorZ > kPortHeight)
					continue;

				const double sq = Detail::DistSq(at, box.origin);
				if (port.kind == Port::Kind::Loose || sq < best)
				{
					best       = sq;
					port.kind  = Port::Kind::Building;
					port.index = b;
				}
			}
			return port;
		}

		const Scene&                                          m_scene;
		std::vector<Port>                                     m_start;
		std::map<std::pair<size_t, int32_t>, std::vector<size_t>> m_endsAtPoint;
		std::vector<std::vector<size_t>>                      m_endsAtBuilding;
		Detail::PointGrid                                     m_looseEnds;
	};
}
