layout (location = 0) in vec2 inVertex;
#ifdef TW_TILE_TEXTURED
layout (location = 1) in uvec4 inVertexTexCoord;
#endif

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH(32, vec2 gOffset)
TW_PUSH(40, vec2 gScale)
TW_PUSH_END

#ifdef TW_TILE_TEXTURED
TW_LOC(0) noperspective centroid out vec3 TexCoord;
#endif

void main()
{
	// A tile quad is drawn where it lies, and a border tile is one quad
	// stretched over the area it repeats across. That is the same draw with
	// gOffset at zero and gScale at one, so there is one program for both.
	vec2 VertexPos = (inVertex * TW_PC(gScale)) + TW_PC(gOffset);
	gl_Position = vec4(TW_PC(gPos) * vec4(VertexPos, 0.0, 1.0), 0.0, 1.0);

#ifdef TW_TILE_TEXTURED
	// The tile is repeated as far as the quad was stretched, and a rotated
	// tile repeats along the other axis.
	vec2 TexScale = TW_PC(gScale);
	if (float(inVertexTexCoord.w) > 0.0)
		TexScale = TW_PC(gScale).yx;
	TexCoord = vec3(vec2(inVertexTexCoord.xy) * TexScale, float(inVertexTexCoord.z));
#endif
}
