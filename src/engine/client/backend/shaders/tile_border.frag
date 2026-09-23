#ifdef TW_TILE_TEXTURED
TW_SAMPLER(0, 0) uniform sampler2DArray gTextureSampler;
#endif

TW_PUSH_BEGIN
TW_PUSH(64, vec4 gVertColor)
TW_PUSH_END

#ifdef TW_TILE_TEXTURED
TW_LOC(0) noperspective centroid in vec3 TexCoord;
#endif

TW_LOC(0) out vec4 FragClr;

void main()
{
#ifdef TW_TILE_TEXTURED
	vec3 realTexCoords = vec3(fract(TexCoord.xy), TexCoord.z);
	vec2 dx = dFdx(TexCoord.xy);
	vec2 dy = dFdy(TexCoord.xy);
	vec4 tex = textureGrad(gTextureSampler, realTexCoords, dx, dy);
	FragClr = tex * TW_PC(gVertColor);
#else
	FragClr = TW_PC(gVertColor);
#endif
}
