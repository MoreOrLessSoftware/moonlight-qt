// Copies PyroWave's decoded Y plane into the luma plane of an NV12/P010 frame
Texture2D<float> lumaPlane : register(t0);

float main(float4 pos : SV_POSITION) : SV_TARGET
{
    return lumaPlane.Load(int3(pos.xy, 0));
}
