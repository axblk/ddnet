#ifdef TW_TILE_TEXTURED
TW_SAMPLER(0, 0) uniform sampler2DArray gTextureSampler;
#endif

TW_PUSH_BEGIN
TW_PUSH(64, vec4 gVertColor)
TW_PUSH_END

#ifdef TW_TILE_TEXTURED
TW_LOC(0) noperspective in vec3 TexCoord;
#endif

TW_LOC(0) out vec4 FragClr;
void main()
{
#ifdef TW_TILE_TEXTURED
	vec4 TexColor = texture(gTextureSampler, TexCoord.xyz);
	FragClr = TexColor * TW_PC(gVertColor);
#else
	FragClr = TW_PC(gVertColor);
#endif
}
