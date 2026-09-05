layout (location = 0) in vec2 inVertex;
layout (location = 1) in vec4 inVertexColor;
layout (location = 2) in vec3 inVertexTexCoord;

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH_END

TW_LOC(0) noperspective out vec4 oVertColor;
#ifdef TW_TEXTURED
TW_LOC(1) noperspective out vec3 oTexCoord;
#endif

void main()
{
	gl_Position = vec4(TW_PC(gPos) * vec4(inVertex, 0.0, 1.0), 0.0, 1.0);
#ifdef TW_TEXTURED
	oTexCoord = inVertexTexCoord;
#endif
	oVertColor = inVertexColor;
}
