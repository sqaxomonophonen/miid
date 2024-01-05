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

static inline void order_2i32(int* i0, int* i1)
{
	if (*i0 < *i1) return;
	const int tmp = *i0;
	*i0 = *i1;
	*i1 = tmp;
}

static inline void order_2f32(float* f0, float* f1)
{
	if (*f0 < *f1) return;
	const int tmp = *f0;
	*f0 = *f1;
	*f1 = tmp;
}

static inline void order_3f32(float* f0, float* f1, float* f2)
{
	order_2f32(f0, f1);
	order_2f32(f1, f2);
	order_2f32(f0, f1);
}

#define UTIL_H
#endif
