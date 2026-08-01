#pragma once

#include "OpenXRHeaders.h"
#include "quaternion.h"
#include "vector3d.h"

static constexpr XrQuaternionf IdentityQuat = {0, 0, 0, 1};
static constexpr XrVector3f IdentityVec = {0, 0, 0};
static constexpr XrPosef IdentityPose = { IdentityQuat, IdentityVec };

static inline core::vector3df xr_to_irrlicht(const XrVector3f& pos)
{
	return core::vector3df(pos.x, pos.y, -pos.z);
}

static inline core::quaternion xr_to_irrlicht(const XrQuaternionf& q)
{
	return core::quaternion(q.x, q.y, -q.z, q.w);
}

static inline core::pose xr_to_irrlicht(const XrPosef& pose)
{
	return core::pose(
		xr_to_irrlicht(pose.position),
		xr_to_irrlicht(pose.orientation));
}

static inline XrQuaternionf irrlicht_to_xr(const core::quaternion& q)
{
	XrQuaternionf result;
	result.x = q.X;
	result.y = q.Y;
	result.z = -q.Z;
	result.w = q.W;
	return result;
}

static inline XrVector3f irrlicht_to_xr(const core::vector3df& v)
{
	XrVector3f result;
	result.x = v.X;
	result.y = v.Y;
	result.z = -v.Z;
	return result;
}

static inline XrExtent2Df irrlicht_to_xr(const core::dimension2df& v)
{
	XrExtent2Df result;
	result.width = v.Width;
	result.height = v.Height;
	return result;
}

// Multiply quaternions
static inline XrQuaternionf quatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
        XrQuaternionf result;
        result.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
        result.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
        result.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
        result.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
        return result;
}

// Invert a quaternion
static inline XrQuaternionf quatInv(const XrQuaternionf& a)
{
	XrQuaternionf result = {
		.x = -a.x,
		.y = -a.y,
		.z = -a.z,
		.w = a.w,
	};
	return result;
}

// Apply quaternion 'a' as a rotation to vector 'b'
static inline XrVector3f quatApply(const XrQuaternionf& a, const XrVector3f &b)
{
        XrQuaternionf bquat = {
		.x = b.x,
		.y = b.y,
		.z = b.z,
		.w = 0,
	};
	XrQuaternionf r = quatMul(quatMul(a, bquat), quatInv(a));
	XrVector3f result = {
		.x = r.x,
		.y = r.y,
		.z = r.z,
	};
	return result;
}

static inline XrVector3f vecScale(float k, const XrVector3f& a)
{
	XrVector3f result = {
		.x = k * a.x,
		.y = k * a.y,
		.z = k * a.z,
	};
	return result;
}

static inline XrVector3f vecAdd(const XrVector3f& a, const XrVector3f& b)
{
	XrVector3f result = {
		.x = a.x + b.x,
		.y = a.y + b.y,
		.z = a.z + b.z,
	};
	return result;
}

static inline XrVector3f vecSub(const XrVector3f& a, const XrVector3f& b)
{
	XrVector3f result = {
		.x = a.x - b.x,
		.y = a.y - b.y,
		.z = a.z - b.z,
	};
	return result;
}

static inline float vecLengthSq(const XrVector3f& a)
{
	return a.x * a.x + a.y * a.y + a.z * a.z;
}

static inline float vecLength(const XrVector3f& a)
{
	return sqrt(vecLengthSq(a));
}

static inline XrQuaternionf quatNormalize(const XrQuaternionf& a)
{
	float invlen = 1.0 / sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
	XrQuaternionf result;
	result.x = a.x * invlen;
	result.y = a.y * invlen;
	result.z = a.z * invlen;
	result.w = a.w * invlen;
	return result;
}

// Compose two poses
static inline XrPosef poseMul(const XrPosef& a, const XrPosef& b)
{
        XrPosef result;
        result.orientation = quatNormalize(quatMul(a.orientation, b.orientation));
        result.position = vecAdd(a.position, quatApply(a.orientation, b.position));
	return result;
}
