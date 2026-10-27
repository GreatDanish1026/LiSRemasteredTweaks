// Graded HDR: turns the game's finished SDR frame (colour grade, UI and all) into HDR10.
// Compiled by build.ps1 with fxc (vs_5_0 / ps_5_0); DXBC runs on DirectX 11 and 12.
//
// The SDR image is decoded with gamma 2.2 (how it looks on an SDR monitor) or the sRGB curve,
// placed at the chosen paper white, and only its top end is stretched towards the chosen peak:
// below the knee nothing changes, so the grade and the shadows stay as Deck Nine made them.
// Hue is kept by scaling RGB together. Then Rec. 709 -> Rec. 2020 and the ST 2084 (PQ) curve.

cbuffer Params : register(b0)
{
    float paperWhite;  // nits for SDR white (1.0)
    float peak;        // nits the brightest SDR white is stretched to (<= paperWhite: no stretch)
    float knee;        // relative luminance where the stretch starts (0..1)
    float gamma;       // decode: > 0 = power gamma, 0 = sRGB curve
};

Texture2D<float4> source : register(t0);

void main_vs(uint id : SV_VertexID, out float4 position : SV_Position)
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 SrgbToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToPq(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(max(nits / 10000.0, 0.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

static const float3x3 kRec709ToRec2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

float4 main_ps(float4 position : SV_Position) : SV_Target
{
    const float3 c = saturate(source.Load(int3(position.xy, 0)).rgb);
    float3 rgb = gamma > 0 ? pow(c, gamma) : SrgbToLinear(c);

    // Highlight stretch on luminance: y -> knee + (y - knee) * (1 + s * t^2), t = (y - knee) / (1 - knee).
    // Continuous with slope 1 at the knee; SDR white (1.0) lands on peak / paperWhite.
    const float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    const float top = peak / paperWhite;
    if (top > 1 && y > knee)
    {
        const float t = (y - knee) / (1 - knee);
        const float stretched = knee + (y - knee) * (1 + (top - 1) / (1 - knee) * t * t);
        rgb *= stretched / max(y, 1e-5);
    }

    const float3 rec2020 = mul(kRec709ToRec2020, rgb);
    return float4(LinearToPq(max(rec2020, 0) * paperWhite), 1);
}
