/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOSSERVER_LAUNCHGUARD_H
#define MANGOSSERVER_LAUNCHGUARD_H

#include "Movement/MoveSplineInitArgs.h"

#include <atomic>

class Unit;

namespace Movement
{
    // How a player is moved: a spline launched by any movement generator or effect, or a near teleport.
    enum class LaunchKind : uint8 { Spline, Teleport };

    // A check a module may set on every move of a player before it happens: the final path a spline is launched with
    // (its first point the player's place), or a near teleport's {here, there}. False refuses it: the spline is not
    // launched, the teleport not made, and the player's MotionMaster removes the generator that asked for it (Refused).
    // Unset, nothing is checked. It is called on the player's map thread, or on the world thread between map updates.
    typedef bool (*LaunchGuardFn)(Unit& unit, PointsArray const& path, LaunchKind kind);

    inline std::atomic<LaunchGuardFn>& LaunchGuardSlot()
    {
        static std::atomic<LaunchGuardFn> slot{ nullptr };
        return slot;
    }

    inline void SetLaunchGuard(LaunchGuardFn guard) { LaunchGuardSlot().store(guard); }
    inline LaunchGuardFn GetLaunchGuard() { return LaunchGuardSlot().load(std::memory_order_relaxed); }
}

#endif
