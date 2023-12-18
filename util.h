#ifndef UTIL_H

#include "imgui.h"

#define ARRAY_LENGTH(xs) (sizeof(xs) / sizeof((xs)[0]))

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

#define UTIL_H
#endif
