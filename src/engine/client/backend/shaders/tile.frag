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
	// coordinates count cells from 0 and are wrapped here: the sampler of the
	// tile array clamps, and wrapping in it would reach into the neighbouring
	// layer. A pixel on the edge of the quad may be interpolated a hair past
	// it, where fract would sample the opposite edge of the tile - a line
	// along the quad. So the cell is taken a little below the coordinate and
	// never below the first one, and the coordinate is clamped into it. The
	// implicit derivatives break at the cell border, so they are taken from
	// the unwrapped coordinates.
	vec2 dx = dFdx(TexCoord.xy);
	vec2 dy = dFdy(TexCoord.xy);
	vec2 Margin = max((abs(dx) + abs(dy)) / 1024.0, vec2(1.0 / 16384.0));
	vec2 Cell = max(floor(TexCoord.xy - Margin), vec2(0.0));
	vec3 RealTexCoords = vec3(clamp(TexCoord.xy - Cell, 0.0, 1.0), TexCoord.z);
	vec4 TexColor = textureGrad(gTextureSampler, RealTexCoords, dx, dy);
	FragClr = TexColor * TW_PC(gVertColor);
#else
	FragClr = TW_PC(gVertColor);
#endif
}
