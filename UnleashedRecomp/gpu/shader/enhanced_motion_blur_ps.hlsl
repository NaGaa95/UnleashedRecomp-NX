#include "../../../tools/XenosRecomp/XenosRecomp/shader_common.h"

#ifdef __spirv__

// Constants: from the set 4 uniform buffers when the pipeline has SPEC_CONSTANT_CONSTANTS_UBO (NVK reads
// them from a hardware constant bank), otherwise through the push-constant pointers, as before.
#define g_BlurRate (UR_CONSTANTS_UBO ? g_UboPixel.v[150] : vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 2400, 0x10))
#define g_ViewportSize (UR_CONSTANTS_UBO ? g_UboPixel.v[24] : vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 384, 0x10))

#define sampColor_Texture2DDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(0) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0))
#define sampColor_SamplerDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(192) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192))

#define sampVelocityMap_Texture2DDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 4))
#define sampVelocityMap_SamplerDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(196) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 196))

#define sampZBuffer_Texture2DDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(8) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 8))
#define sampZBuffer_SamplerDescriptorIndex (UR_CONSTANTS_UBO ? UR_SHARED_UINT(200) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 200))

#else

cbuffer PixelShaderConstants : register(b1, space4)
{
    float4 g_BlurRate : packoffset(c150);
    float4 g_ViewportSize : packoffset(c24);
};

cbuffer SharedConstants : register(b2, space4)
{
    uint sampColor_Texture2DDescriptorIndex : packoffset(c0.x);
    uint sampColor_SamplerDescriptorIndex : packoffset(c12.x);
    
    uint sampVelocityMap_Texture2DDescriptorIndex : packoffset(c0.y);
    uint sampVelocityMap_SamplerDescriptorIndex : packoffset(c12.y);
    
    uint sampZBuffer_Texture2DDescriptorIndex : packoffset(c0.z);
    uint sampZBuffer_SamplerDescriptorIndex : packoffset(c12.z);
    
	DEFINE_SHARED_CONSTANTS();
};

#endif

float4 main(in float4 position : SV_Position, in float4 texCoord : TEXCOORD0) : SV_Target
{
    Texture2D<float4> sampColor = g_Texture2DDescriptorHeap[sampColor_Texture2DDescriptorIndex];
    Texture2D<float4> sampVelocityMap = g_Texture2DDescriptorHeap[sampVelocityMap_Texture2DDescriptorIndex];
    Texture2D<float4> sampZBuffer = g_Texture2DDescriptorHeap[sampZBuffer_Texture2DDescriptorIndex];
    
    SamplerState sampColor_s = g_SamplerDescriptorHeap[sampColor_SamplerDescriptorIndex]; 
    SamplerState sampVelocityMap_s = g_SamplerDescriptorHeap[sampVelocityMap_SamplerDescriptorIndex];   
    SamplerState sampZBuffer_s = g_SamplerDescriptorHeap[sampZBuffer_SamplerDescriptorIndex];
    
    float depth = sampZBuffer.SampleLevel(sampZBuffer_s, texCoord.xy, 0).x;
    float4 velocityMap = sampVelocityMap.SampleLevel(sampVelocityMap_s, texCoord.xy, 0);
    float2 velocity = (velocityMap.xz + velocityMap.yw / 255.0) * 2.0 - 1.0;

    int sampleCount = min(64, round(length(velocity * g_ViewportSize.xy)));
    float2 sampleOffset = velocity / (float) sampleCount;

    float3 color = sampColor.SampleLevel(sampColor_s, texCoord.xy, 0).rgb;
    int count = 1;

    for (int i = 1; i <= sampleCount; i++)
    {
        float2 sampleCoord = texCoord.xy + sampleOffset * i;
        float3 sampleColor = sampColor.SampleLevel(sampColor_s, sampleCoord, 0).rgb;
        float sampleDepth = sampZBuffer.SampleLevel(sampZBuffer_s, sampleCoord, 0).x;

        if (sampleDepth - depth < 0.01)
        {
            color += sampleColor;
            count += 1;
        }
    }

    return float4(color / count, g_BlurRate.x * saturate(dot(abs(velocity), g_ViewportSize.xy) / 8.0) * saturate(count - 1));
}
