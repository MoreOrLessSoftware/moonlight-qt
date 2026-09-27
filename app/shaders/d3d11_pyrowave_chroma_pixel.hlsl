// Interleaves PyroWave's decoded Cb and Cr planes into the chroma plane of an NV12/P010 frame
Texture2D<float> cbPlane : register(t0);
Texture2D<float> crPlane : register(t1);

float2 main(float4 pos : SV_POSITION) : SV_TARGET
{
    int3 coord = int3(pos.xy, 0);
    return float2(cbPlane.Load(coord), crPlane.Load(coord));
}
