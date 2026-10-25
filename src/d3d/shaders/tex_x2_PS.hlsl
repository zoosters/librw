// textured pixel shader with the D3D8 stage 0 combiner COLOROP = MODULATE2X
// (texture * diffuse * 2), alpha = texture * diffuse as before.
// texGain.x is 2.0 (default) or 1.0 (PORT_NOMODULATE2X=1, plain modulate as librw).
struct VS_out {
	float4 Position		: POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float4 Color		: COLOR0;
};

sampler2D tex0 : register(s0);

float4 fogColor : register(c0);
float4 texGain : register(c2);

float4 main(VS_out input) : COLOR
{
	float4 color = input.Color;
	float4 t = tex2D(tex0, input.TexCoord0.xy);
	color.rgb = color.rgb*t.rgb*texGain.x;
	color.a *= t.a;
	color.rgb = lerp(fogColor.rgb, color.rgb, input.TexCoord0.z);
	return color;
}
