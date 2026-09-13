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
	// A quad may cover more than one cell and then repeats its tile, so the
	// coordinates count cells and are wrapped here: the sampler of the tile
	// array clamps, and wrapping in it would reach into the neighbouring
	// layer. fract breaks the implicit derivatives at the seam, so they are
	// taken from the unwrapped coordinates - the same way tile_border.frag
	// has always done it.
	vec3 RealTexCoords = vec3(fract(TexCoord.xy), TexCoord.z);
	vec2 dx = dFdx(TexCoord.xy);
	vec2 dy = dFdy(TexCoord.xy);
	vec4 TexColor = textureGrad(gTextureSampler, RealTexCoords, dx, dy);
	FragClr = TexColor * TW_PC(gVertColor);
#else
	FragClr = TW_PC(gVertColor);
#endif
}
