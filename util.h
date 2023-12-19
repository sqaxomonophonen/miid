#ifndef UTIL_H

#include <stdlib.h>
#include <string.h>

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

static inline ImVec4 imvec4_scale(ImVec4 a, float s)
{
	return ImVec4(
		a.x * s,
		a.y * s,
		a.z * s,
		a.w * s);
}


static inline ImVec4 imvec4_lerp(ImVec4 a, ImVec4 b, float t)
{
	return imvec4_add(a, imvec4_scale(imvec4_sub(b, a), t));
}

static inline char* copystring(char* src)
{
	const size_t sz = strlen(src);
	char* dst = (char*)malloc(sz+1);
	memcpy(dst, src, sz);
	dst[sz] = 0;
	return dst;
}


#define UTIL_H
#endif
