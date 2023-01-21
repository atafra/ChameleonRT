#include "util.hlsl"

RWTexture2D<float4> framebuffer : register(u0);

RWStructuredBuffer<float4> color_buffer : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
  const uint2 pixel = dispatchThreadId.xy;
  uint2 dims;
  framebuffer.GetDimensions(dims.x, dims.y);
  if (pixel.x >= dims.x || pixel.y >= dims.y)
    return;

  const uint pixel_index = dims.x * pixel.y + pixel.x;
  const float4 color = color_buffer[pixel_index];
  framebuffer[pixel] = float4(linear_to_srgb(color.r),
                              linear_to_srgb(color.g),
                              linear_to_srgb(color.b), 1.f);
}