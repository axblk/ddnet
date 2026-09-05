layout (location = 0) in vec4 inVertex;
layout (location = 1) in vec4 inColor;
#ifdef TW_QUAD_TEXTURED
layout (location = 2) in vec2 inVertexTexCoord;
#endif

// How many quads one draw carries. OpenGL says at run time what its uniform
// limit allows; Vulkan has room for 256 in its uniform buffer.
#ifndef TW_MAX_QUADS
#define TW_MAX_QUADS 256
#endif

#ifdef TW_VULKAN
struct SQuadUniformEl {
	vec4 gVertColor;
	vec2 gOffset;
	float gRotation;
};
#endif

TW_PUSH_BEGIN
TW_PUSH(0, mat4x2 gPos)
#if defined(TW_VULKAN) && defined(TW_QUAD_GROUPED)
TW_PUSH(32, SQuadUniformEl gUniEls[1])
#endif
#ifndef TW_QUAD_GROUPED
TW_PUSH(32, int gQuadOffset)
#endif
TW_PUSH_END

// Offset and rotation per quad. Vulkan keeps them in a struct array, in the
// push constants for a group of one and in a uniform buffer otherwise;
// OpenGL keeps two uniform arrays. QUAD_OFFSET and QUAD_ROTATION hide that.
#ifdef TW_VULKAN
#ifdef TW_QUAD_GROUPED
#define QUAD_EL(Index) TW_PC(gUniEls)[Index]
#else
#ifdef TW_QUAD_TEXTURED
#define UBOSetIndex 1
#else
#define UBOSetIndex 0
#endif
layout (std140, set = UBOSetIndex, binding = 1) uniform SOffBO {
	SQuadUniformEl gUniEls[TW_MAX_QUADS];
} gQuadBO;
#define QUAD_EL(Index) gQuadBO.gUniEls[Index]
#endif
#define QUAD_OFFSET(Index) QUAD_EL(Index).gOffset
#define QUAD_ROTATION(Index) QUAD_EL(Index).gRotation
#else
#ifdef TW_QUAD_GROUPED
uniform vec2 gOffsets[1];
uniform float gRotations[1];
#else
uniform vec2 gOffsets[TW_MAX_QUADS];
uniform float gRotations[TW_MAX_QUADS];
#endif
#define QUAD_OFFSET(Index) gOffsets[Index]
#define QUAD_ROTATION(Index) gRotations[Index]
#endif

TW_LOC(0) noperspective out vec4 QuadColor;
#ifndef TW_QUAD_GROUPED
TW_LOC(1) flat out int QuadIndex;
#ifdef TW_QUAD_TEXTURED
TW_LOC(2) noperspective out vec2 TexCoord;
#endif
#else
#ifdef TW_QUAD_TEXTURED
TW_LOC(1) noperspective out vec2 TexCoord;
#endif
#endif

void main()
{
	vec2 FinalPos = vec2(inVertex.xy);

#ifndef TW_QUAD_GROUPED
	int TmpQuadIndex = int(TW_VERTEX_ID / 4) - TW_PC(gQuadOffset);
#else
#define TmpQuadIndex 0
#endif
	if(QUAD_ROTATION(TmpQuadIndex) != 0.0)
	{
		float X = FinalPos.x - inVertex.z;
		float Y = FinalPos.y - inVertex.w;

		FinalPos.x = X * cos(QUAD_ROTATION(TmpQuadIndex)) - Y * sin(QUAD_ROTATION(TmpQuadIndex)) + inVertex.z;
		FinalPos.y = X * sin(QUAD_ROTATION(TmpQuadIndex)) + Y * cos(QUAD_ROTATION(TmpQuadIndex)) + inVertex.w;
	}
	FinalPos += QUAD_OFFSET(TmpQuadIndex);

#ifndef TW_QUAD_GROUPED
	QuadIndex = TmpQuadIndex;
#endif

	gl_Position = vec4(TW_PC(gPos) * vec4(FinalPos, 0.0, 1.0), 0.0, 1.0);
	QuadColor = inColor;

#ifdef TW_QUAD_TEXTURED
	TexCoord = inVertexTexCoord;
#endif
}
