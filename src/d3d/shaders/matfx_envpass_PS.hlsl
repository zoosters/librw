// second pass of RW 3.6's D3D8 MatFX environment map (generic path):
// fixed function stage 0 = env texture MODULATE tfactor (coefficient byte replicated), alpha = env texture alpha, blend SRCALPHA / ONE, no z write,
// lighting off (the vertex colour is not used), fog colour black (additive pass).
struct VS_out {
	float4 Position		: POSITION;
	float3 TexCoord0	: TEXCOORD0;
	float2 TexCoord1	: TEXCOORD1;
	float4 Color		: COLOR0;
	float4 EnvColor		: COLOR1;
};

sampler2D envTex : register(s1);

float4 fogColor : register(c0);
float4 fxparams : register(c1);

#define coef (fxparams.x)

float4 main(VS_out input) : COLOR
{
	float4 e = tex2D(envTex, input.TexCoord1.xy);
	float4 color;
	color.rgb = e.rgb * coef * input.TexCoord0.z;
	color.a = e.a;
	return color;
}
