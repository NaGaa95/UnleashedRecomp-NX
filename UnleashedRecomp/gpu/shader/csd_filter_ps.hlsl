#include "../../../tools/XenosRecomp/XenosRecomp/shader_common.h"

#ifdef __spirv__

// Constants: from the set 4 uniform buffers when the pipeline has SPEC_CONSTANT_CONSTANTS_UBO (NVK reads
// them from a hardware constant bank), otherwise through the push-constant pointers, as before.
#define s0_Texture2DDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(0) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0))
#define s0_SamplerDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(192) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192))
// Size of the texture in slot 0 (SPEC_CONSTANT_TEXTURE_SIZE; float2 at byte 288).
#define s0_Texture2DSize (UR_CONSTANTS_UBO ? float2(UR_SHARED_FLOAT(288), UR_SHARED_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))

#else

cbuffer SharedConstants : register(b2, space4)
{
    uint s0_Texture2DDescriptorIndex : packoffset(c0.x);
    uint s0_SamplerDescriptorIndex : packoffset(c12.x);
	DEFINE_SHARED_CONSTANTS();
};

#endif

float4 main(
	in float4 iPosition : SV_Position,
	in float4 iTexCoord0 : TEXCOORD0,
	in float4 iTexCoord1 : TEXCOORD1) : SV_Target
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[s0_Texture2DDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[s0_SamplerDescriptorIndex];
    
#ifdef __spirv__
    // The size table holds exactly what GetDimensions() returns (verify mode keeps the query).
    float2 dimensions = getTexture2DSize(texture, s0_Texture2DSize);
#else
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
#endif
    
    // https://www.shadertoy.com/view/csX3RH
    float2 uvTexspace = iTexCoord1.xy * dimensions;
    float2 seam = floor(uvTexspace + 0.5);
    uvTexspace = (uvTexspace - seam) / fwidth(uvTexspace) + seam;
    uvTexspace = clamp(uvTexspace, seam - 0.5, seam + 0.5);
    float2 texCoord = uvTexspace / dimensions;
    
    float4 color = texture.Sample(samplerState, texCoord);
    color *= iTexCoord0;
    
    // The game enables alpha test for CSD, but the alpha threshold doesn't seem to be assigned anywhere? Weird.
    clip(color.a - g_AlphaThreshold);
    
    return color;
}
