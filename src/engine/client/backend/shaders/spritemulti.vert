layout (location = 0) in vec2 inVertex;
layout (location = 1) in vec2 inVertexTexCoord;
layout (location = 2) in vec4 inVertexColor;

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
TW_PUSH(32, vec2 gCenter)
#if defined(TW_VULKAN) && defined(TW_PUSH_CONST)
TW_PUSH(48, vec4 gRSP[1])
#endif
TW_PUSH_END

// Rotation, scale and position per instance. Vulkan reads them from a
// uniform buffer, or for a single sprite from the push constants; OpenGL
// from a uniform array as large as its uniform limit allows.
#ifdef TW_VULKAN
#ifdef TW_PUSH_CONST
#define RSP(Instance) TW_PC(gRSP)[0]
#else
layout (std140, set = 1, binding = 1) uniform SRSPBO {
	vec4 gRSP[512];
} gRSPBO;
#define RSP(Instance) gRSPBO.gRSP[Instance]
#endif
#else
uniform vec4 gRSP[228];
#define RSP(Instance) gRSP[Instance]
#endif

TW_LOC(0) noperspective out vec2 texCoord;
TW_LOC(1) noperspective out vec4 vertColor;

void main()
{
	vec2 FinalPos = vec2(inVertex.xy);
	if(RSP(TW_INSTANCE_ID).w != 0.0)
	{
		float X = FinalPos.x - TW_PC(gCenter).x;
		float Y = FinalPos.y - TW_PC(gCenter).y;

		FinalPos.x = X * cos(RSP(TW_INSTANCE_ID).w) - Y * sin(RSP(TW_INSTANCE_ID).w) + TW_PC(gCenter).x;
		FinalPos.y = X * sin(RSP(TW_INSTANCE_ID).w) + Y * cos(RSP(TW_INSTANCE_ID).w) + TW_PC(gCenter).y;
	}

	FinalPos.x *= RSP(TW_INSTANCE_ID).z;
	FinalPos.y *= RSP(TW_INSTANCE_ID).z;

	FinalPos.x += RSP(TW_INSTANCE_ID).x;
	FinalPos.y += RSP(TW_INSTANCE_ID).y;

	gl_Position = vec4(TW_PC(gPos) * vec4(FinalPos, 0.0, 1.0), 0.0, 1.0);
	texCoord = inVertexTexCoord;
	vertColor = inVertexColor;
}
