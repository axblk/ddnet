#ifdef TW_TEXTURED
TW_SAMPLER(0, 0) uniform sampler2DArray gTextureSampler;
#endif

TW_LOC(0) noperspective in vec4 oVertColor;
#ifdef TW_TEXTURED
TW_LOC(1) noperspective in vec3 oTexCoord;
#endif

TW_LOC(0) out vec4 FragClr;

void main()
{
#ifdef TW_TEXTURED
	vec4 TexColor = texture(gTextureSampler, oTexCoord.xyz).rgba;
	FragClr = TexColor.rgba * oVertColor.rgba;
#else
	FragClr = oVertColor.rgba;
#endif
}
