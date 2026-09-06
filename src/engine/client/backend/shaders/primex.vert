layout (location = 0) in vec2 inVertex;
layout (location = 1) in vec2 inVertexTexCoord;
layout (location = 2) in vec4 inVertexColor;

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH(32, vec2 gCenter)
TW_PUSH(40, float gRotation)
TW_PUSH_END

TW_LOC(0) noperspective out vec2 texCoord;
TW_LOC(1) noperspective out vec4 vertColor;

void main()
{
	vec2 FinalPos = vec2(inVertex.xy);
	float X = FinalPos.x - TW_PC(gCenter).x;
	float Y = FinalPos.y - TW_PC(gCenter).y;

	FinalPos.x = X * cos(TW_PC(gRotation)) - Y * sin(TW_PC(gRotation)) + TW_PC(gCenter).x;
	FinalPos.y = X * sin(TW_PC(gRotation)) + Y * cos(TW_PC(gRotation)) + TW_PC(gCenter).y;

	gl_Position = vec4(TW_PC(gPos) * vec4(FinalPos, 0.0, 1.0), 0.0, 1.0);
	texCoord = inVertexTexCoord;
	vertColor = inVertexColor;
}
