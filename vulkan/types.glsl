#ifndef TYPES_GLSL
#define TYPES_GLSL

// No typedefs GLSL?
#define uint32_t uint

struct uint3 {
	uint x, y, z; 
};

struct float3 {
	float x, y, z;
};

struct float2 {
	float x, y;
};

struct RayPayload {
    vec3 normal;
    float dist;
    vec2 uv;
    uint material_id;
    float pad;
};

#endif

