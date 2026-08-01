/*
Minetest
Copyright (C) 2024 Minetest Authors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#pragma once

#include "irrTypes.h"
#include "irrMath.h"
#include "matrix4.h"
#include "vector3d.h"
#include "pose.h"
#include <ostream>

namespace core
{

//! A pose is a position and rotation which represents a local coordinate system.

/* In XR, every physical object (HMD, controllers, etc) has a pose relative to another space.
   It also makes sense to talk about relative poses (e.g. pose of the controller relative to the HMD). */
class pose
{
	public:

		//! Default Constructor
		pose() {}

		//! Constructor
		pose(const vector3df& position, const quaternion& rotation)
			: Position(position)
			, Rotation(rotation) { }

		//! Transform this pose into the local space of `B`.
		pose relativeTo(const pose& B) const
		{
			return B.inverse() * (*this);
		}

		//! Transform a point in local space to world space
		vector3df transformPoint(const vector3df& v) const
		{
			return Position + (Rotation * v);
		}

		//! Transform a vector in local space to world space.
		vector3df transformVector(const vector3df& v) const
		{
			return (Rotation * v);
		}

		//! Transform a local pose to world space
		//! Note that:
		//!   A.transformPoint(B.transformPoint(P)) == (A.transformPose(B)).transformPoint(P)
		pose transformPose(const pose& B) const
		{
			return pose(
				Position + (Rotation * B.Position),
				B.Rotation * Rotation);
		}

		//! Same as transformPose
		pose operator*(const pose& other) const
		{
			return transformPose(other);
		}

		//! Multiplication operator
		pose& operator*=(const pose& other)
		{
			Position += Rotation * other.Position;
			Rotation = other.Rotation * Rotation;
			return *this;
		}

		//! Inverse pose
		pose inverse() const
		{
			quaternion invRotation = Rotation.inverse();
			return pose(
				invRotation * (-Position),
				invRotation);
		}

		//! pose elements.
		vector3df Position;
		quaternion Rotation;
};

} // end namespace core

inline std::ostream& operator<<(std::ostream& os, const core::pose& pose)
{
	os << "pose(" << pose.Position << "," << pose.Rotation << ")";
	return os;
}

