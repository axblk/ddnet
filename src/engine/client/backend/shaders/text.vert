layout (location = 0) in vec2 inVertex;
layout (location = 1) in vec2 inVertexTexCoord;
layout (location = 2) in vec4 inVertexColor;

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH(32, float gTextureSize)
TW_PUSH_END

TW_LOC(0) noperspective out vec2 texCoord;
TW_LOC(1) noperspective out vec4 outVertColor;

void main()
{
	gl_Position = vec4(TW_PC(gPos) * vec4(inVertex, 0.0, 1.0), 0.0, 1.0);

	texCoord = vec2(inVertexTexCoord.x / TW_PC(gTextureSize), inVertexTexCoord.y / TW_PC(gTextureSize));
	outVertColor = inVertexColor;
}
