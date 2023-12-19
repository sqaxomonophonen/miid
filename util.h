#ifndef UTIL_H

#include "imgui.h"

#define ARRAY_LENGTH(xs) (sizeof(xs) / sizeof((xs)[0]))

static inline float lerp(float a, float b, float t)
{
	return a + t*(b-a);
}

static inline ImVec4 imvec4_add(ImVec4 a, ImVec4 b)
{
	return ImVec4(
		a.x + b.x,
		a.y + b.y,
		a.z + b.z,
		a.w + b.w);
}

static inline ImVec4 imvec4_sub(ImVec4 a, ImVec4 b)
{
	return ImVec4(
		a.x - b.x,
		a.y - b.y,
		a.z - b.z,
		a.w - b.w);
}

static inline ImVec4 imvec4_mul(ImVec4 a, ImVec4 b)
{
	return ImVec4(
		a.x * b.x,
		a.y * b.y,
		a.z * b.z,
		a.w * b.w);
}

static inline ImVec4 imvec4_lerp(ImVec4 a, ImVec4 b, float t)
{
	return imvec4_add(a, imvec4_mul(ImVec4(t,t,t,t), imvec4_sub(b, a)));
}

#define UTIL_H
#endif
