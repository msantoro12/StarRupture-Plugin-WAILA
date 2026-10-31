#pragma once

// The drone rail topology, in plain C++ with no game types, so the same code
// runs in the plugin and in Tests\RailNetworkTests.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Waila::RailNetwork
{
	struct Vec3
	{
		double x = 0.0, y = 0.0, z = 0.0;
	};

	// The world positions of a rail's first and last spline point.
	struct Rail
	{
		Vec3 start;
		Vec3 end;
	};

	// A crafter's actor bounds.
	struct Crafter
	{
		Vec3 origin;
		Vec3 extent;
	};

	struct Scene
	{
		std::vector<Rail>    rails;
		std::vector<Crafter> crafters;
	};

	struct Network
	{
		std::vector<size_t> crafters;   // indices into Scene::crafters
		std::vector<size_t> rails;      // indices into Scene::rails, the start rail first
		bool                truncated = false;
	};

	// INFERRED, as is everything about which buildings a rail serves: the SDK
	// has no explicit building-to-rail link. ConnectedStartActor and
	// ConnectedEndActor were null on every rail probed in a loaded save, so the
	// topology comes from the rail splines instead. Two rails are neighbours when
	// an end of one lies within kJoinTolerance of an end of the other. A crafter is
	// a source when a rail end lies inside its bounds grown by kCrafterMargin, and
	// the fill does not continue through that end. A building that only consumes
	// from the rail cannot be told apart from one that feeds it.
	constexpr double kJoinTolerance = 100.0;   // unreal units, 100 = 1 m
	constexpr double kCrafterMargin = 200.0;   // unreal units, 200 = 2 m

	namespace Detail
	{
		inline bool InsideBox(const Vec3& p, const Crafter& box)
		{
			return std::abs(p.x - box.origin.x) <= box.extent.x + kCrafterMargin
			    && std::abs(p.y - box.origin.y) <= box.extent.y + kCrafterMargin
			    && std::abs(p.z - box.origin.z) <= box.extent.z + kCrafterMargin;
		}

		// Rail ends are bucketed in a grid of kJoinTolerance cells, so finding the ends
		// near a point only checks the 27 cells around it.
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

		inline Cell CellOf(const Vec3& p)
		{
			return { static_cast<int64_t>(std::floor(p.x / kJoinTolerance)),
			         static_cast<int64_t>(std::floor(p.y / kJoinTolerance)),
			         static_cast<int64_t>(std::floor(p.z / kJoinTolerance)) };
		}

		struct RailEnd
		{
			size_t rail;
			Vec3   at;
		};
	}

	// Flood fill over the rail segments, from `start` outwards. Rails meet where
	// their spline ends do, so two rails are neighbours when an end of one lies
	// within kJoinTolerance of an end of the other.
	// An end that no other rail meets and that lies inside a crafter's bounds
	// makes that crafter a source. Nothing continues from such an end, so a
	// factory's input and output rails do not merge into one network. Rails only
	// join end to end: the sockets of a junction, roundabout or vertical connector
	// sit metres apart, so the fill does not cross those buildings.
	inline Network CollectNetwork(const Scene& scene, size_t start, size_t maxRails)
	{
		using namespace Detail;

		Network out;
		if (start >= scene.rails.size())
			return out;

		std::unordered_map<Cell, std::vector<RailEnd>, CellHash> grid;
		for (size_t i = 0; i < scene.rails.size(); ++i)
		{
			grid[CellOf(scene.rails[i].start)].push_back({ i, scene.rails[i].start });
			grid[CellOf(scene.rails[i].end)].push_back({ i, scene.rails[i].end });
		}

		std::vector<bool>   seen(scene.rails.size(), false);
		std::vector<size_t> queue{ start };
		seen[start] = true;
		std::unordered_set<size_t> direct;

		const double toleranceSq = kJoinTolerance * kJoinTolerance;

		for (size_t head = 0; head < queue.size(); ++head)
		{
			const size_t current = queue[head];

			for (const Vec3& at : { scene.rails[current].start, scene.rails[current].end })
			{
				// The other rails whose ends meet this one.
				std::vector<size_t> meets;
				const Cell c = CellOf(at);
				for (int64_t dx = -1; dx <= 1; ++dx)
				for (int64_t dy = -1; dy <= 1; ++dy)
				for (int64_t dz = -1; dz <= 1; ++dz)
				{
					const auto it = grid.find({ c.x + dx, c.y + dy, c.z + dz });
					if (it == grid.end())
						continue;

					for (const RailEnd& other : it->second)
					{
						if (other.rail == current)
							continue;

						const double ex = at.x - other.at.x, ey = at.y - other.at.y, ez = at.z - other.at.z;
						if (ex * ex + ey * ey + ez * ez <= toleranceSq)
							meets.push_back(other.rail);
					}
				}

				// An end nothing else meets is where the rail reaches a building.
				// It serves the one crafter whose bounds hold it, and the fill stops.
				if (meets.empty())
				{
					size_t nearest   = scene.crafters.size();
					double nearestSq = 0.0;
					for (size_t b = 0; b < scene.crafters.size(); ++b)
					{
						const Crafter& box = scene.crafters[b];
						if (!InsideBox(at, box))
							continue;

						const double bx = at.x - box.origin.x, by = at.y - box.origin.y, bz = at.z - box.origin.z;
						const double sq = bx * bx + by * by + bz * bz;
						if (nearest == scene.crafters.size() || sq < nearestSq)
						{
							nearest   = b;
							nearestSq = sq;
						}
					}
					if (nearest != scene.crafters.size() && direct.insert(nearest).second)
						out.crafters.push_back(nearest);
					continue;
				}

				for (size_t next : meets)
				{
					if (seen[next])
						continue;

					if (queue.size() >= maxRails)
					{
						out.truncated = true;
						out.rails = std::move(queue);
						return out;
					}
					seen[next] = true;
					queue.push_back(next);
				}
			}
		}

		out.rails = std::move(queue);
		return out;
	}
}
