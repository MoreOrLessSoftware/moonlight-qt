// Packs PyroWave's decoded Y, Cb and Cr planes into a 4:4:4 frame in AYUV's VUYA order,
// in an ordinary BGRA or R10G10B10A2 texture since not every GPU supports AYUV/Y410 ones
Texture2D<float> yPlane : register(t0);
Texture2D<float> cbPlane : register(t1);
Texture2D<float> crPlane : register(t2);

float4 main(float4 pos : SV_POSITION) : SV_TARGET
{
    int3 coord = int3(pos.xy, 0);
    return float4(crPlane.Load(coord), cbPlane.Load(coord), yPlane.Load(coord), 1.0);
}
