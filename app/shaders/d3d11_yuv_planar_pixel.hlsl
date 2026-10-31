// Y, Cb and Cr each in a plane of its own, as PyroWave decodes them, for 4:2:0 and 4:4:4
Texture2D<min16float> yPlane : register(t0);
Texture2D<min16float> cbPlane : register(t1);
Texture2D<min16float> crPlane : register(t2);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer CSC_CONST_BUF : register(b0)
{
    min16float3x3 cscMatrix;
    min16float3 offsets;
    min16float2 chromaOffset; // Zero for 4:4:4
    min16float2 chromaTexMax;
};

min16float4 main(ShaderInput input) : SV_TARGET
{
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    float2 chromaTex = min(input.tex + chromaOffset, chromaTexMax);
    min16float3 yuv = min16float3(yPlane.Sample(theSampler, input.tex),
                                  cbPlane.Sample(theSampler, chromaTex),
                                  crPlane.Sample(theSampler, chromaTex));

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return min16float4(yuv, 1.0);
}
