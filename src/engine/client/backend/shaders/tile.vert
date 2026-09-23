layout (location = 0) in vec2 inVertex;
#ifdef TW_TILE_TEXTURED
layout (location = 1) in uvec4 inVertexTexCoord;
#endif

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH_END

#ifdef TW_TILE_TEXTURED
TW_LOC(0) noperspective out vec3 TexCoord;
#endif

void main()
{
	gl_Position = vec4(TW_PC(gPos) * vec4(inVertex, 0.0, 1.0), 0.0, 1.0);

#ifdef TW_TILE_TEXTURED
	TexCoord = vec3(inVertexTexCoord.xyz);
#endif
}
