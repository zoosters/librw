uniform sampler2D tex0;

FSIN vec4 v_color;
FSIN vec2 v_tex0;
FSIN float v_fog;

void
main(void)
{
	vec4 color = v_color*texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y));
	color.rgb *= 1.0 + u_alphaRef.z;	// zoosters: MODULATE2X for textured draws
	if(u_alphaRef.w > 0.5) color.a = v_color.a*(1.0 - texture(tex0, vec2(v_tex0.x, 1.0-v_tex0.y)).a);	// zoosters: complement alpha
	color.rgb = mix(u_fogColor.rgb, color.rgb, v_fog);
	DoAlphaTest(color.a);
	FRAGCOLOR(color);
}
