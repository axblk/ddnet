#include "backend_opengl3.h"

#include <base/detect.h>
#include <base/log.h>
#include <base/mem.h>

#if defined(BACKEND_AS_OPENGL_ES) || !defined(CONF_BACKEND_OPENGL_ES)

#ifndef BACKEND_AS_OPENGL_ES
#include <GL/glew.h>
#else
#if defined(CONF_PLATFORM_IOS)
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#include <GLES3/gl3.h>
#endif
#endif

#include <engine/client/backend/glsl_shader_compiler.h>
#include <engine/client/backend/opengl/opengl_sl.h>
#include <engine/client/backend/opengl/opengl_sl_program.h>
#include <engine/client/backend_threaded.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/shared/config.h>

#include <algorithm>
#include <memory>

// WebGL2 defines the type of a buffer at the first bind to a buffer target, and
// keeps it for the life of the buffer. This is different to GLES 3 and desktop
// GL (https://www.khronos.org/registry/webgl/specs/latest/2.0/#5.1), where a
// buffer is what it is used as. A buffer that indices are read from therefore
// has to be born as an index buffer and be uploaded to as one ever after.
static GLenum BufferInitTarget([[maybe_unused]] IGraphics::EBufferUsage Usage)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	return Usage == IGraphics::EBufferUsage::INDEX ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
#else
	return GL_COPY_WRITE_BUFFER;
#endif
}

// The element array binding belongs to the vertex array object, so uploading to
// an index buffer through it would silently replace what the container that is
// bound draws with. Nothing is drawn from a container here, and every draw binds
// its own, so the upload goes through no container at all.
static void BindBufferObject(GLenum Target, GLuint Buffer)
{
	if(Target == GL_ELEMENT_ARRAY_BUFFER)
		glBindVertexArray(0);
	glBindBuffer(Target, Buffer);
}

static GLenum VertexAttributeTypeToOpenGL(IGraphics::EVertexAttributeType Type)
{
	switch(Type)
	{
	case IGraphics::EVertexAttributeType::FLOAT32: return GL_FLOAT;
	case IGraphics::EVertexAttributeType::UINT8: return GL_UNSIGNED_BYTE;
	}
	dbg_assert_failed("invalid vertex attribute type");
	return GL_FLOAT;
}

// ------------ CCommandProcessorFragment_OpenGL3_3
void CCommandProcessorFragment_OpenGL3_3::UseProgram(CGLSLTWProgram *pProgram)
{
	if(m_LastProgramId != pProgram->GetProgramId())
	{
		pProgram->UseProgram();
		m_LastProgramId = pProgram->GetProgramId();
	}
}

void CCommandProcessorFragment_OpenGL3_3::InitPrimExProgram(CGLSLPrimitiveExProgram *pProgram, CGLSLCompiler *pCompiler, bool Textured)
{
	CGLSL PrimitiveVertexShader;
	CGLSL PrimitiveFragmentShader;
	if(Textured)
		pCompiler->AddDefine("TW_TEXTURED", "");
	PrimitiveVertexShader.LoadShader(pCompiler, "primex.vert", GL_VERTEX_SHADER);
	PrimitiveFragmentShader.LoadShader(pCompiler, "primex.frag", GL_FRAGMENT_SHADER);
	if(Textured)
		pCompiler->ClearDefines();

	pProgram->CreateProgram();
	pProgram->AddShader(&PrimitiveVertexShader);
	pProgram->AddShader(&PrimitiveFragmentShader);
	pProgram->LinkProgram();

	UseProgram(pProgram);

	pProgram->m_LocPos = pProgram->GetUniformLoc("gPos");
	pProgram->m_LocTextureSampler = pProgram->GetUniformLoc("gTextureSampler");
	pProgram->m_LocRotation = pProgram->GetUniformLoc("gRotation");
	pProgram->m_LocCenter = pProgram->GetUniformLoc("gCenter");
	pProgram->m_LocVertciesColor = pProgram->GetUniformLoc("gVerticesColor");

	pProgram->SetUniform(pProgram->m_LocRotation, 0.0f);
	float aCenter[2] = {0.f, 0.f};
	pProgram->SetUniformVec2(pProgram->m_LocCenter, 1, aCenter);
}

bool CCommandProcessorFragment_OpenGL3_3::Cmd_Init(const SCommand_Init *pCommand)
{
	if(!InitOpenGL(pCommand))
		return false;

	m_OpenGLTextureLodBIAS = g_Config.m_GfxGLTextureLODBIAS;

	glActiveTexture(GL_TEXTURE0);

	m_Has2DArrayTextures = true;
	m_2DArrayTarget = GL_TEXTURE_2D_ARRAY;
	m_Has3DTextures = false;
	m_HasMipMaps = true;
	m_HasNPOTTextures = true;
	m_HasShaders = true;

	m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
	m_pTextureMemoryUsage->store(0, std::memory_order_relaxed);
	m_LastBlendMode = EBlendMode::ALPHA;
	m_LastClipEnable = false;
	m_pPrimitiveProgram = new CGLSLPrimitiveProgram;
	m_pPrimitiveProgramTextured = new CGLSLPrimitiveProgram;
	m_pPlanarYuvProgram = new CGLSLPrimitiveProgram;
	m_pTileProgram = new CGLSLTileProgram;
	m_pTileProgramTextured = new CGLSLTileProgram;
	m_pPrimitive3DProgram = new CGLSLPrimitiveProgram;
	m_pPrimitive3DProgramTextured = new CGLSLPrimitiveProgram;
	m_pBorderTileProgram = new CGLSLTileProgram;
	m_pBorderTileProgramTextured = new CGLSLTileProgram;
	m_pQuadProgram = new CGLSLQuadProgram;
	m_pQuadProgramTextured = new CGLSLQuadProgram;
	m_pQuadProgramGrouped = new CGLSLQuadProgram;
	m_pQuadProgramTexturedGrouped = new CGLSLQuadProgram;
	m_pTextProgram = new CGLSLTextProgram;
	m_pPrimitiveExProgram = new CGLSLPrimitiveExProgram;
	m_pPrimitiveExProgramTextured = new CGLSLPrimitiveExProgram;
	m_pSpriteProgramMultiple = new CGLSLSpriteMultipleProgram;
	m_LastProgramId = 0;

	CGLSLCompiler ShaderCompiler(g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch, m_IsOpenGLES, m_OpenGLTextureLodBIAS / 1000.0f);

	GLint CapVal;
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &CapVal);

	m_MaxQuadsAtOnce = std::min(((int)CapVal - 20) / (3 * 4), (int)ms_MaxQuadsPossible);

	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "prim.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, "prim.frag", GL_FRAGMENT_SHADER);

		m_pPrimitiveProgram->CreateProgram();
		m_pPrimitiveProgram->AddShader(&PrimitiveVertexShader);
		m_pPrimitiveProgram->AddShader(&PrimitiveFragmentShader);
		m_pPrimitiveProgram->LinkProgram();

		UseProgram(m_pPrimitiveProgram);

		m_pPrimitiveProgram->m_LocPos = m_pPrimitiveProgram->GetUniformLoc("gPos");
		m_pPrimitiveProgram->m_LocTextureSampler = m_pPrimitiveProgram->GetUniformLoc("gTextureSampler");
	}
	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		ShaderCompiler.AddDefine("TW_TEXTURED", "");
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "prim.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, "prim.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pPrimitiveProgramTextured->CreateProgram();
		m_pPrimitiveProgramTextured->AddShader(&PrimitiveVertexShader);
		m_pPrimitiveProgramTextured->AddShader(&PrimitiveFragmentShader);
		m_pPrimitiveProgramTextured->LinkProgram();

		UseProgram(m_pPrimitiveProgramTextured);

		m_pPrimitiveProgramTextured->m_LocPos = m_pPrimitiveProgramTextured->GetUniformLoc("gPos");
		m_pPrimitiveProgramTextured->m_LocTextureSampler = m_pPrimitiveProgramTextured->GetUniformLoc("gTextureSampler");
	}
	{
		CGLSL PrimitiveVertexShader;
		CGLSL PlanarYuvFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "prim.vert", GL_VERTEX_SHADER);
		PlanarYuvFragmentShader.LoadShader(&ShaderCompiler, "planar_yuv.frag", GL_FRAGMENT_SHADER);

		m_pPlanarYuvProgram->CreateProgram();
		m_pPlanarYuvProgram->AddShader(&PrimitiveVertexShader);
		m_pPlanarYuvProgram->AddShader(&PlanarYuvFragmentShader);
		m_pPlanarYuvProgram->LinkProgram();

		UseProgram(m_pPlanarYuvProgram);

		m_pPlanarYuvProgram->m_LocPos = m_pPlanarYuvProgram->GetUniformLoc("gPos");
		m_pPlanarYuvProgram->m_LocTextureSampler = m_pPlanarYuvProgram->GetUniformLoc("gTextureSampler");
	}

	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "prim3d.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, "prim3d.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pPrimitive3DProgram->CreateProgram();
		m_pPrimitive3DProgram->AddShader(&PrimitiveVertexShader);
		m_pPrimitive3DProgram->AddShader(&PrimitiveFragmentShader);
		m_pPrimitive3DProgram->LinkProgram();

		UseProgram(m_pPrimitive3DProgram);

		m_pPrimitive3DProgram->m_LocPos = m_pPrimitive3DProgram->GetUniformLoc("gPos");
	}
	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		ShaderCompiler.AddDefine("TW_TEXTURED", "");
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "prim3d.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, "prim3d.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pPrimitive3DProgramTextured->CreateProgram();
		m_pPrimitive3DProgramTextured->AddShader(&PrimitiveVertexShader);
		m_pPrimitive3DProgramTextured->AddShader(&PrimitiveFragmentShader);
		m_pPrimitive3DProgramTextured->LinkProgram();

		UseProgram(m_pPrimitive3DProgramTextured);

		m_pPrimitive3DProgramTextured->m_LocPos = m_pPrimitive3DProgramTextured->GetUniformLoc("gPos");
		m_pPrimitive3DProgramTextured->m_LocTextureSampler = m_pPrimitive3DProgramTextured->GetUniformLoc("gTextureSampler");
	}

	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		VertexShader.LoadShader(&ShaderCompiler, "tile.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "tile.frag", GL_FRAGMENT_SHADER);

		m_pTileProgram->CreateProgram();
		m_pTileProgram->AddShader(&VertexShader);
		m_pTileProgram->AddShader(&FragmentShader);
		m_pTileProgram->LinkProgram();

		UseProgram(m_pTileProgram);

		m_pTileProgram->m_LocPos = m_pTileProgram->GetUniformLoc("gPos");
		m_pTileProgram->m_LocColor = m_pTileProgram->GetUniformLoc("gVertColor");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_TILE_TEXTURED", "");
		VertexShader.LoadShader(&ShaderCompiler, "tile.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "tile.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pTileProgramTextured->CreateProgram();
		m_pTileProgramTextured->AddShader(&VertexShader);
		m_pTileProgramTextured->AddShader(&FragmentShader);
		m_pTileProgramTextured->LinkProgram();

		UseProgram(m_pTileProgramTextured);

		m_pTileProgramTextured->m_LocPos = m_pTileProgramTextured->GetUniformLoc("gPos");
		m_pTileProgramTextured->m_LocTextureSampler = m_pTileProgramTextured->GetUniformLoc("gTextureSampler");
		m_pTileProgramTextured->m_LocColor = m_pTileProgramTextured->GetUniformLoc("gVertColor");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		VertexShader.LoadShader(&ShaderCompiler, "tile_border.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "tile_border.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pBorderTileProgram->CreateProgram();
		m_pBorderTileProgram->AddShader(&VertexShader);
		m_pBorderTileProgram->AddShader(&FragmentShader);
		m_pBorderTileProgram->LinkProgram();

		UseProgram(m_pBorderTileProgram);

		m_pBorderTileProgram->m_LocPos = m_pBorderTileProgram->GetUniformLoc("gPos");
		m_pBorderTileProgram->m_LocColor = m_pBorderTileProgram->GetUniformLoc("gVertColor");
		m_pBorderTileProgram->m_LocOffset = m_pBorderTileProgram->GetUniformLoc("gOffset");
		m_pBorderTileProgram->m_LocScale = m_pBorderTileProgram->GetUniformLoc("gScale");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_TILE_TEXTURED", "");
		VertexShader.LoadShader(&ShaderCompiler, "tile_border.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "tile_border.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pBorderTileProgramTextured->CreateProgram();
		m_pBorderTileProgramTextured->AddShader(&VertexShader);
		m_pBorderTileProgramTextured->AddShader(&FragmentShader);
		m_pBorderTileProgramTextured->LinkProgram();

		UseProgram(m_pBorderTileProgramTextured);

		m_pBorderTileProgramTextured->m_LocPos = m_pBorderTileProgramTextured->GetUniformLoc("gPos");
		m_pBorderTileProgramTextured->m_LocTextureSampler = m_pBorderTileProgramTextured->GetUniformLoc("gTextureSampler");
		m_pBorderTileProgramTextured->m_LocColor = m_pBorderTileProgramTextured->GetUniformLoc("gVertColor");
		m_pBorderTileProgramTextured->m_LocOffset = m_pBorderTileProgramTextured->GetUniformLoc("gOffset");
		m_pBorderTileProgramTextured->m_LocScale = m_pBorderTileProgramTextured->GetUniformLoc("gScale");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_MAX_QUADS", std::to_string(m_MaxQuadsAtOnce).c_str());
		VertexShader.LoadShader(&ShaderCompiler, "quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "quad.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pQuadProgram->CreateProgram();
		m_pQuadProgram->AddShader(&VertexShader);
		m_pQuadProgram->AddShader(&FragmentShader);
		m_pQuadProgram->LinkProgram();

		UseProgram(m_pQuadProgram);

		m_pQuadProgram->m_LocPos = m_pQuadProgram->GetUniformLoc("gPos");
		m_pQuadProgram->m_LocColors = m_pQuadProgram->GetUniformLoc("gVertColors");
		m_pQuadProgram->m_LocRotations = m_pQuadProgram->GetUniformLoc("gRotations");
		m_pQuadProgram->m_LocOffsets = m_pQuadProgram->GetUniformLoc("gOffsets");
		m_pQuadProgram->m_LocQuadOffset = m_pQuadProgram->GetUniformLoc("gQuadOffset");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_QUAD_TEXTURED", "");
		ShaderCompiler.AddDefine("TW_MAX_QUADS", std::to_string(m_MaxQuadsAtOnce).c_str());
		VertexShader.LoadShader(&ShaderCompiler, "quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "quad.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pQuadProgramTextured->CreateProgram();
		m_pQuadProgramTextured->AddShader(&VertexShader);
		m_pQuadProgramTextured->AddShader(&FragmentShader);
		m_pQuadProgramTextured->LinkProgram();

		UseProgram(m_pQuadProgramTextured);

		m_pQuadProgramTextured->m_LocPos = m_pQuadProgramTextured->GetUniformLoc("gPos");
		m_pQuadProgramTextured->m_LocTextureSampler = m_pQuadProgramTextured->GetUniformLoc("gTextureSampler");
		m_pQuadProgramTextured->m_LocColors = m_pQuadProgramTextured->GetUniformLoc("gVertColors");
		m_pQuadProgramTextured->m_LocRotations = m_pQuadProgramTextured->GetUniformLoc("gRotations");
		m_pQuadProgramTextured->m_LocOffsets = m_pQuadProgramTextured->GetUniformLoc("gOffsets");
		m_pQuadProgramTextured->m_LocQuadOffset = m_pQuadProgramTextured->GetUniformLoc("gQuadOffset");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_QUAD_GROUPED", "");
		VertexShader.LoadShader(&ShaderCompiler, "quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "quad.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pQuadProgramGrouped->CreateProgram();
		m_pQuadProgramGrouped->AddShader(&VertexShader);
		m_pQuadProgramGrouped->AddShader(&FragmentShader);
		m_pQuadProgramGrouped->LinkProgram();

		UseProgram(m_pQuadProgramGrouped);

		m_pQuadProgramGrouped->m_LocPos = m_pQuadProgramGrouped->GetUniformLoc("gPos");
		m_pQuadProgramGrouped->m_LocColors = m_pQuadProgramGrouped->GetUniformLoc("gVertColors");
		m_pQuadProgramGrouped->m_LocRotations = m_pQuadProgramGrouped->GetUniformLoc("gRotations");
		m_pQuadProgramGrouped->m_LocOffsets = m_pQuadProgramGrouped->GetUniformLoc("gOffsets");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		ShaderCompiler.AddDefine("TW_QUAD_TEXTURED", "");
		ShaderCompiler.AddDefine("TW_QUAD_GROUPED", "");
		VertexShader.LoadShader(&ShaderCompiler, "quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "quad.frag", GL_FRAGMENT_SHADER);
		ShaderCompiler.ClearDefines();

		m_pQuadProgramTexturedGrouped->CreateProgram();
		m_pQuadProgramTexturedGrouped->AddShader(&VertexShader);
		m_pQuadProgramTexturedGrouped->AddShader(&FragmentShader);
		m_pQuadProgramTexturedGrouped->LinkProgram();

		UseProgram(m_pQuadProgramTexturedGrouped);

		m_pQuadProgramTexturedGrouped->m_LocPos = m_pQuadProgramTexturedGrouped->GetUniformLoc("gPos");
		m_pQuadProgramTexturedGrouped->m_LocTextureSampler = m_pQuadProgramTexturedGrouped->GetUniformLoc("gTextureSampler");
		m_pQuadProgramTexturedGrouped->m_LocColors = m_pQuadProgramTexturedGrouped->GetUniformLoc("gVertColors");
		m_pQuadProgramTexturedGrouped->m_LocRotations = m_pQuadProgramTexturedGrouped->GetUniformLoc("gRotations");
		m_pQuadProgramTexturedGrouped->m_LocOffsets = m_pQuadProgramTexturedGrouped->GetUniformLoc("gOffsets");
	}
	{
		CGLSL VertexShader;
		CGLSL FragmentShader;
		VertexShader.LoadShader(&ShaderCompiler, "text.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, "text.frag", GL_FRAGMENT_SHADER);

		m_pTextProgram->CreateProgram();
		m_pTextProgram->AddShader(&VertexShader);
		m_pTextProgram->AddShader(&FragmentShader);
		m_pTextProgram->LinkProgram();

		UseProgram(m_pTextProgram);

		m_pTextProgram->m_LocPos = m_pTextProgram->GetUniformLoc("gPos");
		m_pTextProgram->m_LocTextureSampler = -1;
		m_pTextProgram->m_LocTextSampler = m_pTextProgram->GetUniformLoc("gTextSampler");
		m_pTextProgram->m_LocColor = m_pTextProgram->GetUniformLoc("gVertColor");
		m_pTextProgram->m_LocOutlineColor = m_pTextProgram->GetUniformLoc("gVertOutlineColor");
		m_pTextProgram->m_LocTextureSize = m_pTextProgram->GetUniformLoc("gTextureSize");
	}
	InitPrimExProgram(m_pPrimitiveExProgram, &ShaderCompiler, false);
	InitPrimExProgram(m_pPrimitiveExProgramTextured, &ShaderCompiler, true);
	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, "spritemulti.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, "spritemulti.frag", GL_FRAGMENT_SHADER);

		m_pSpriteProgramMultiple->CreateProgram();
		m_pSpriteProgramMultiple->AddShader(&PrimitiveVertexShader);
		m_pSpriteProgramMultiple->AddShader(&PrimitiveFragmentShader);
		m_pSpriteProgramMultiple->LinkProgram();

		UseProgram(m_pSpriteProgramMultiple);

		m_pSpriteProgramMultiple->m_LocPos = m_pSpriteProgramMultiple->GetUniformLoc("gPos");
		m_pSpriteProgramMultiple->m_LocTextureSampler = m_pSpriteProgramMultiple->GetUniformLoc("gTextureSampler");
		m_pSpriteProgramMultiple->m_LocRSP = m_pSpriteProgramMultiple->GetUniformLoc("gRSP[0]");
		m_pSpriteProgramMultiple->m_LocCenter = m_pSpriteProgramMultiple->GetUniformLoc("gCenter");
		m_pSpriteProgramMultiple->m_LocVertciesColor = m_pSpriteProgramMultiple->GetUniformLoc("gVerticesColor");

		float aCenter[2] = {0.f, 0.f};
		m_pSpriteProgramMultiple->SetUniformVec2(m_pSpriteProgramMultiple->m_LocCenter, 1, aCenter);
	}

	m_LastStreamBuffer = 0;

	glGenBuffers(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawBufferId);
	glGenBuffers(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawIndexBufferId);
	glGenVertexArrays(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawVertexId);
	glGenBuffers(1, &m_PrimitiveDrawBufferIdTex3D);
	glGenVertexArrays(1, &m_PrimitiveDrawVertexIdTex3D);

	for(int i = 0; i < MAX_STREAM_BUFFER_COUNT; ++i)
	{
		glBindBuffer(GL_ARRAY_BUFFER, m_aPrimitiveDrawBufferId[i]);
		glBindVertexArray(m_aPrimitiveDrawVertexId[i]);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glEnableVertexAttribArray(2);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertex), nullptr);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertex), (void *)(sizeof(float) * 2));
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(CCommandBuffer::SVertex), (void *)(sizeof(float) * 4));

		m_aLastIndexBufferBound[i] = 0;
	}

	glBindBuffer(GL_ARRAY_BUFFER, m_PrimitiveDrawBufferIdTex3D);
	glBindVertexArray(m_PrimitiveDrawVertexIdTex3D);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertexTex3DStream), nullptr);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(CCommandBuffer::SVertexTex3DStream), (void *)(sizeof(float) * 2));
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertexTex3DStream), (void *)(sizeof(float) * 2 + sizeof(unsigned char) * 4));

	// query the image max size only once
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_MaxTexSize);

	glBindVertexArray(0);

	m_vTextures.resize(CCommandBuffer::MAX_TEXTURES);

	m_ClearColor.r = m_ClearColor.g = m_ClearColor.b = -1.f;

	// fix the alignment to allow even 1byte changes, e.g. for alpha components
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	return true;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Shutdown(const SCommand_Shutdown *pCommand)
{
	// Whoever is waiting for a picture has to be told, and the device is
	// still there to finish it.
	FinishReadbacks();
	glUseProgram(0);

	// clean up everything
	delete m_pPrimitiveProgram;
	delete m_pPrimitiveProgramTextured;
	delete m_pPlanarYuvProgram;
	delete m_pBorderTileProgram;
	delete m_pBorderTileProgramTextured;
	delete m_pQuadProgram;
	delete m_pQuadProgramTextured;
	delete m_pQuadProgramGrouped;
	delete m_pQuadProgramTexturedGrouped;
	delete m_pTileProgram;
	delete m_pTileProgramTextured;
	delete m_pPrimitive3DProgram;
	delete m_pPrimitive3DProgramTextured;
	delete m_pTextProgram;
	delete m_pPrimitiveExProgram;
	delete m_pPrimitiveExProgramTextured;
	delete m_pSpriteProgramMultiple;

	glBindVertexArray(0);
	glDeleteBuffers(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawBufferId);
	glDeleteBuffers(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawIndexBufferId);
	glDeleteVertexArrays(MAX_STREAM_BUFFER_COUNT, m_aPrimitiveDrawVertexId);
	glDeleteBuffers(1, &m_PrimitiveDrawBufferIdTex3D);
	glDeleteVertexArrays(1, &m_PrimitiveDrawVertexIdTex3D);

	for(int i = 0; i < (int)m_vTextures.size(); ++i)
	{
		DestroyTexture(i);
	}

	for(size_t i = 0; i < m_vVertexArrays.size(); ++i)
	{
		DestroyVertexArray(i);
	}

	m_vVertexArrays.clear();
}

int CCommandProcessorFragment_OpenGL3_3::ToGLFormat(IGraphics::ETextureFormat Format)
{
	switch(Format)
	{
	case IGraphics::ETextureFormat::RGBA8_UNORM: return GL_RGBA;
	case IGraphics::ETextureFormat::RG8_UNORM: return GL_RG;
	case IGraphics::ETextureFormat::R8_UNORM: return GL_RED;
	}
	dbg_assert(false, "Unknown texture format");
	dbg_break();
}

void CCommandProcessorFragment_OpenGL3_3::TextureUpdate(int Slot, int X, int Y, int Width, int Height, IGraphics::ETextureFormat Format, uint8_t *pTexData)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);

	if(m_vTextures[Slot].m_RescaleCount > 0)
	{
		const int OldWidth = Width;
		const int OldHeight = Height;
		for(int i = 0; i < m_vTextures[Slot].m_RescaleCount; ++i)
		{
			Width >>= 1;
			Height >>= 1;

			X /= 2;
			Y /= 2;
		}

		pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, IGraphics::PixelSize(Format)));
		pTexData = pOwnedTexData.get();
	}

	glTexSubImage2D(GL_TEXTURE_2D, 0, X, Y, Width, Height, ToGLFormat(Format), GL_UNSIGNED_BYTE, pTexData);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
{
	int Slot = 0;
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindSampler(Slot, 0);
	DestroyTexture(pCommand->m_Texture.Id());
}

void CCommandProcessorFragment_OpenGL3_3::TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	int Width = Desc.m_Width;
	int Height = Desc.m_Height;
	const int GLFormat = ToGLFormat(Desc.m_Format);
	int GLStoreFormat = GLFormat;
	while(Slot >= (int)m_vTextures.size())
		m_vTextures.resize(m_vTextures.size() * 2);
	m_vTextures[Slot].m_SourceWidth = Desc.m_Width;
	m_vTextures[Slot].m_SourceHeight = Desc.m_Height;
	m_vTextures[Slot].m_Format = Desc.m_Format;
	m_vTextures[Slot].m_Usage = Desc.m_Usage;

	// resample if needed
	int RescaleCount = 0;
	if(GLFormat == GL_RGBA && pTexData != nullptr)
	{
		if(Width > m_MaxTexSize || Height > m_MaxTexSize)
		{
			const int OldWidth = Width;
			const int OldHeight = Height;
			do
			{
				Width >>= 1;
				Height >>= 1;
				++RescaleCount;
			} while(Width > m_MaxTexSize || Height > m_MaxTexSize);

			pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, IGraphics::PixelSize(Desc.m_Format)));
			pTexData = pOwnedTexData.get();
		}
	}
	m_vTextures[Slot].m_Width = Width;
	m_vTextures[Slot].m_Height = Height;
	m_vTextures[Slot].m_RescaleCount = RescaleCount;

	if(GLStoreFormat == GL_RED)
		GLStoreFormat = GL_R8;
	const size_t PixelSize = IGraphics::PixelSize(Desc.m_Format);

	int SamplerSlot = 0;

	if(Desc.m_Create2D)
	{
		glGenTextures(1, &m_vTextures[Slot].m_Tex);
		glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);

		glGenSamplers(1, &m_vTextures[Slot].m_Sampler);
		glBindSampler(SamplerSlot, m_vTextures[Slot].m_Sampler);
	}

	if(Desc.m_Mipmaps == IGraphics::ETextureMipmaps::NONE)
	{
		if(Desc.m_Create2D)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
		}
	}
	else
	{
		if(Desc.m_Create2D)
		{
			glSamplerParameteri(m_vTextures[Slot].m_Sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

#ifndef BACKEND_AS_OPENGL_ES
			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glSamplerParameterf(m_vTextures[Slot].m_Sampler, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));
#endif

			// prevent mipmap display bugs, when zooming out far
			if(Width >= 1024 && Height >= 1024)
			{
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 5.f);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 5);
			}
			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
			glGenerateMipmap(GL_TEXTURE_2D);
		}

		if(Desc.m_Layering == IGraphics::ETextureLayering::LAYERED)
		{
			const int LayerColumns = Desc.m_LayerColumns;
			const int LayerRows = Desc.m_LayerRows;
			const int LayerCount = static_cast<int>(Desc.LayerCount());
			glGenTextures(1, &m_vTextures[Slot].m_Tex2DArray);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_vTextures[Slot].m_Tex2DArray);

			glGenSamplers(1, &m_vTextures[Slot].m_Sampler2DArray);
			glBindSampler(SamplerSlot, m_vTextures[Slot].m_Sampler2DArray);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

#ifndef BACKEND_AS_OPENGL_ES
			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glSamplerParameterf(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));
#endif

			int Image3DWidth, Image3DHeight;
			std::unique_ptr<uint8_t, decltype(&free)> pImageData3D = PrepareLayeredImage(pTexData, Width, Height, PixelSize, LayerColumns, LayerRows, Image3DWidth, Image3DHeight);
			dbg_assert(pImageData3D != nullptr, "Failed to allocate 2D array texture conversion memory");
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GLStoreFormat, Image3DWidth, Image3DHeight, LayerCount, 0, GLFormat, GL_UNSIGNED_BYTE, pImageData3D.get());
			glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
		}
	}

	if(Desc.HasUsage(IGraphics::TEXTURE_USAGE_COLOR_TARGET))
	{
		GLint PreviousFramebuffer = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &PreviousFramebuffer);
		glGenFramebuffers(1, &m_vTextures[Slot].m_Framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, m_vTextures[Slot].m_Framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vTextures[Slot].m_Tex, 0);
		const GLenum Status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_FRAMEBUFFER, PreviousFramebuffer);
		if(Status != GL_FRAMEBUFFER_COMPLETE)
		{
			log_error("gfx/opengl", "Creating color target framebuffer failed. Status=%u", static_cast<unsigned>(Status));
			glDeleteFramebuffers(1, &m_vTextures[Slot].m_Framebuffer);
			m_vTextures[Slot].m_Framebuffer = 0;
		}
	}

	// This is the initial value for the wrap modes
	m_vTextures[Slot].m_LastWrapMode = EWrapMode::REPEAT;

	// calculate memory usage
	m_vTextures[Slot].m_MemSize = (size_t)Width * Height * PixelSize;
	while(Width > 2 && Height > 2)
	{
		Width >>= 1;
		Height >>= 1;
		m_vTextures[Slot].m_MemSize += (size_t)Width * Height * PixelSize;
	}
	m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) + m_vTextures[Slot].m_MemSize, std::memory_order_relaxed);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
{
	TextureCreate(pCommand->m_Texture.Id(), pCommand->m_Desc, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
{
	if(!IsTextureUpdateValid(pCommand))
		return;

	TextureUpdate(pCommand->m_Texture.Id(), static_cast<int>(pCommand->m_Region.m_X), static_cast<int>(pCommand->m_Region.m_Y), static_cast<int>(pCommand->m_Region.m_Width), static_cast<int>(pCommand->m_Region.m_Height), pCommand->m_Format, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	const CTexture &Texture = m_vTextures[pCommand->m_Texture.Id()];
	if(Texture.m_Framebuffer == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COPY_SOURCE) == 0)
		return;

	while(m_vPendingReadbacks.size() >= READBACK_SLOT_COUNT)
	{
		FinishReadback(m_vPendingReadbacks.front(), true);
		m_vPendingReadbacks.erase(m_vPendingReadbacks.begin());
	}

	SPendingReadback Pending;
	Pending.m_Width = static_cast<uint32_t>(Texture.m_Width);
	Pending.m_Height = static_cast<uint32_t>(Texture.m_Height);
	Pending.m_pResult = pCommand->m_pResult;
	const GLsizeiptr Size = static_cast<GLsizeiptr>(Pending.m_Width) * Pending.m_Height * 4;
	glGenBuffers(1, &Pending.m_PackBuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, Pending.m_PackBuffer);
	glBufferData(GL_PIXEL_PACK_BUFFER, Size, nullptr, GL_STREAM_READ);

	GLint PreviousFramebuffer;
	GLint PackAlignment;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &PreviousFramebuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &PackAlignment);
	glBindFramebuffer(GL_FRAMEBUFFER, Texture.m_Framebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	// With a pack buffer bound this returns at once; the copy happens on the
	// device, behind everything drawn into the target before it.
	glReadPixels(0, 0, Texture.m_Width, Texture.m_Height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glPixelStorei(GL_PACK_ALIGNMENT, PackAlignment);
	glBindFramebuffer(GL_FRAMEBUFFER, PreviousFramebuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	// No row flip, here or when the pixels are collected. Rendering into a
	// texture already runs with the screen rectangle turned upside down
	// (SetState, m_RenderingToTexture), because the result has to be sampled
	// with top-down texture coordinates later. Row zero of what glReadPixels
	// hands back is therefore the top of the picture already, and flipping it
	// again is what turned the in-client export upside down. The presentation
	// readback does flip, and has to: the default framebuffer is not rendered
	// inverted. Vulkan does not flip either, and its render targets are not
	// inverted.

	Pending.m_pFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	// A fence only moves once the commands before it have left for the
	// device, and nothing else in this frame is going to push them out. A
	// driver that refuses a fence gets the old way: everything finished now.
	if(Pending.m_pFence != nullptr)
		glFlush();
	else
		glFinish();
	m_vPendingReadbacks.push_back(Pending);
	// The result is signalled when the pixels have landed, not when this
	// command buffer ends.
	pCommand->m_pCompletion = nullptr;
}

void CCommandProcessorFragment_OpenGL3_3::FinishReadback(SPendingReadback &Pending, bool Wait)
{
	CCommandBuffer::SImageReadbackResult *pResult = Pending.m_pResult;
	auto *pFence = static_cast<GLsync>(Pending.m_pFence);
	if(pFence != nullptr)
	{
		if(Wait)
		{
			// One second at a time, so that a device that stopped answering
			// is a stuck client and not a silent one.
			GLenum Status;
			do
			{
				Status = glClientWaitSync(pFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
			} while(Status == GL_TIMEOUT_EXPIRED);
		}
		glDeleteSync(pFence);
		Pending.m_pFence = nullptr;
	}
	const size_t Size = static_cast<size_t>(Pending.m_Width) * Pending.m_Height * 4;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, Pending.m_PackBuffer);
	const void *pMapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(Size), GL_MAP_READ_BIT);
	if(pMapped != nullptr)
	{
		if(pResult->m_Image.TryReuse(Pending.m_Width, Pending.m_Height, CImageInfo::FORMAT_RGBA))
		{
			mem_copy(pResult->m_Image.m_pData, pMapped, Size);
			pResult->m_Ok = true;
		}
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glDeleteBuffers(1, &Pending.m_PackBuffer);
	Pending.m_PackBuffer = 0;
	if(!pResult->m_Ok)
		log_warn("gfx/opengl", "texture readback failed");
	pResult->Signal();
}

void CCommandProcessorFragment_OpenGL3_3::CollectFinishedReadbacks()
{
	auto It = m_vPendingReadbacks.begin();
	while(It != m_vPendingReadbacks.end())
	{
		// The device finishes them in order, so one that is not there yet
		// means none behind it is either.
		if(It->m_pFence != nullptr && glClientWaitSync(static_cast<GLsync>(It->m_pFence), 0, 0) == GL_TIMEOUT_EXPIRED)
			break;
		FinishReadback(*It, false);
		It = m_vPendingReadbacks.erase(It);
	}
}

void CCommandProcessorFragment_OpenGL3_3::FinishReadbacks()
{
	for(SPendingReadback &Pending : m_vPendingReadbacks)
		FinishReadback(Pending, true);
	m_vPendingReadbacks.clear();
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
{
	// if clip is still active, force disable it for clearing, enable it again afterwards
	bool ClipWasEnabled = m_LastClipEnable;
	if(ClipWasEnabled)
	{
		glDisable(GL_SCISSOR_TEST);
	}
	// The strip under the display cutout is cleared to black; the image is
	// only cleared where it lies.
	const bool ClearAroundCutout = m_HasDisplayCutout && !m_RenderingToTexture;
	if(ClearAroundCutout)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		m_ClearColor.r = m_ClearColor.g = m_ClearColor.b = m_ClearColor.a = 0.0f;
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor(m_PresentationViewportX, m_PresentationViewportY, m_PresentationViewportWidth, m_PresentationViewportHeight);
		glEnable(GL_SCISSOR_TEST);
	}
	if(pCommand->m_Color != m_ClearColor)
	{
		glClearColor(pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a);
		m_ClearColor = pCommand->m_Color;
	}
	glClear(GL_COLOR_BUFFER_BIT);
	if(ClipWasEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
	}
	else if(ClearAroundCutout)
	{
		glDisable(GL_SCISSOR_TEST);
	}
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand)
{
	const IGraphics::CTextureHandle Target = pCommand->m_Desc.m_ColorTarget;
	if(Target.IsValid())
	{
		if(Target.Id() < 0 || static_cast<size_t>(Target.Id()) >= m_vTextures.size())
		{
			Cmd_EndRenderPass(nullptr);
			return;
		}
		const CTexture &Texture = m_vTextures[Target.Id()];
		if(Texture.m_Framebuffer == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0)
		{
			Cmd_EndRenderPass(nullptr);
			return;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, Texture.m_Framebuffer);
		m_RenderTargetWidth = Texture.m_Width;
		m_RenderTargetHeight = Texture.m_Height;
		m_RenderingToTexture = true;
		glViewport(0, 0, m_RenderTargetWidth, m_RenderTargetHeight);
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		m_RenderingToTexture = false;
		glViewport(m_PresentationViewportX, m_PresentationViewportY, m_PresentationViewportWidth, m_PresentationViewportHeight);
	}

	if(pCommand->m_Desc.m_LoadOp == IGraphics::ERenderPassLoadOp::CLEAR)
	{
		CCommandBuffer::SCommand_Clear Clear;
		Clear.m_Color = pCommand->m_Desc.m_ClearColor;
		Clear.m_ForceClear = true;
		Cmd_Clear(&Clear);
	}
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	m_RenderingToTexture = false;
	glViewport(m_PresentationViewportX, m_PresentationViewportY, m_PresentationViewportWidth, m_PresentationViewportHeight);
}

void CCommandProcessorFragment_OpenGL3_3::UploadStreamBufferData(const void *pVertices, size_t DataSize, bool TextureArray)
{
	if(TextureArray)
		glBindBuffer(GL_ARRAY_BUFFER, m_PrimitiveDrawBufferIdTex3D);
	else
		glBindBuffer(GL_ARRAY_BUFFER, m_aPrimitiveDrawBufferId[m_LastStreamBuffer]);

	glBufferData(GL_ARRAY_BUFFER, DataSize, pVertices, GL_STREAM_DRAW);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
	const EPipelineProgram Program = pCommand->m_Program;
	const bool TextureArray = Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY;
	const bool PlanarYuv = Program == EPipelineProgram::PLANAR_YUV;
	if(!TextureArray && Program != EPipelineProgram::PRIMITIVE && !PlanarYuv)
	{
		DropCommand("a transient draw on a pipeline that only indexed draws reach");
		return;
	}
	const uint32_t VerticesPerPrim = VerticesPerPrimitive(pCommand->m_PrimitiveType);
	if(VerticesPerPrim == 0 || pCommand->m_VertexCount == 0 || pCommand->m_VertexCount % VerticesPerPrim != 0)
	{
		DropCommand("a draw whose vertex data does not fit its primitive type");
		return;
	}

	const void *pVertices;
	size_t VertexSize;
	if(TextureArray)
	{
		pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount);
		VertexSize = sizeof(CCommandBuffer::SVertexTex3DStream);
	}
	else
	{
		pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
		VertexSize = sizeof(CCommandBuffer::SVertex);
	}
	if(pVertices == nullptr)
		return;

	const bool Textured = IsTexturedState(pCommand->m_State);
	// The conversion reads the whole source itself, so it takes it as it is
	// and wants nothing blended over it.
	if(PlanarYuv && (!Textured || pCommand->m_State.m_BlendMode != EBlendMode::NONE))
		return;
	CGLSLTWProgram *pProgram;
	if(PlanarYuv)
		pProgram = m_pPlanarYuvProgram;
	else if(Textured)
		pProgram = TextureArray ? static_cast<CGLSLTWProgram *>(m_pPrimitive3DProgramTextured) : m_pPrimitiveProgramTextured;
	else
		pProgram = TextureArray ? static_cast<CGLSLTWProgram *>(m_pPrimitive3DProgram) : m_pPrimitiveProgram;
	UseProgram(pProgram);
	SetState(pCommand->m_State, pProgram, TextureArray);
	UploadStreamBufferData(pVertices, VertexSize * pCommand->m_VertexCount, TextureArray);
	if(TextureArray)
		glBindVertexArray(m_PrimitiveDrawVertexIdTex3D);
	else
		glBindVertexArray(m_aPrimitiveDrawVertexId[m_LastStreamBuffer]);

	switch(pCommand->m_PrimitiveType)
	{
	// We don't support GL_QUADS due to core profile
	case EPrimitiveType::LINES:
		glDrawArrays(GL_LINES, 0, pCommand->m_VertexCount);
		break;
	case EPrimitiveType::TRIANGLES:
		glDrawArrays(GL_TRIANGLES, 0, pCommand->m_VertexCount);
		break;
	case EPrimitiveType::QUADS:
	{
		const TWGLuint IndexBuffer = m_vBufferObjectIndices[pCommand->m_IndexBuffer.Id()];
		if(TextureArray)
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);
		else if(m_aLastIndexBufferBound[m_LastStreamBuffer] != IndexBuffer)
		{
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);
			m_aLastIndexBufferBound[m_LastStreamBuffer] = IndexBuffer;
		}
		glDrawElements(GL_TRIANGLES, pCommand->m_VertexCount / 4 * 6, GL_UNSIGNED_INT, nullptr);
		break;
	}
	default:
		dbg_assert_failed("Invalid primitive type: %d", (int)pCommand->m_PrimitiveType);
	};

	if(!TextureArray)
		m_LastStreamBuffer = (m_LastStreamBuffer + 1 >= MAX_STREAM_BUFFER_COUNT ? 0 : m_LastStreamBuffer + 1);
}

void CCommandProcessorFragment_OpenGL3_3::DestroyVertexArray(size_t Index)
{
	if(Index >= m_vVertexArrays.size())
		return;
	SVertexArray &Array = m_vVertexArrays[Index];
	if(Array.m_VertArrayId != 0)
		glDeleteVertexArrays(1, &Array.m_VertArrayId);
	Array.m_VertArrayId = 0;
	Array.m_LastIndexBufferBound = 0;
	Array.m_Layout = IGraphics::EVertexLayout::COUNT;
}

CCommandProcessorFragment_OpenGL3_3::SVertexArray *CCommandProcessorFragment_OpenGL3_3::VertexArrayFor(IGraphics::CBufferHandle Buffer, IGraphics::EVertexLayout Layout)
{
	if(!Buffer.IsValid() || Layout >= IGraphics::EVertexLayout::COUNT)
		return nullptr;
	const size_t Index = Buffer.Id();
	if(Index >= m_vBufferObjectIndices.size() || m_vBufferObjectIndices[Index] == 0)
		return nullptr;
	if(Index >= m_vVertexArrays.size())
		m_vVertexArrays.resize(Index + 1);
	SVertexArray &Array = m_vVertexArrays[Index];
	if(Array.m_VertArrayId != 0 && Array.m_Layout == Layout)
		return &Array;

	if(Array.m_VertArrayId != 0)
		glDeleteVertexArrays(1, &Array.m_VertArrayId);
	glGenVertexArrays(1, &Array.m_VertArrayId);
	glBindVertexArray(Array.m_VertArrayId);
	Array.m_LastIndexBufferBound = 0;
	Array.m_Layout = Layout;

	const IGraphics::SVertexLayoutDesc &Desc = IGraphics::VertexLayout(Layout);
	glBindBuffer(GL_ARRAY_BUFFER, m_vBufferObjectIndices[Index]);
	for(uint32_t i = 0; i < Desc.m_AttributeCount; ++i)
	{
		const IGraphics::CVertexAttributeDesc &Attr = Desc.m_aAttributes[i];
		glEnableVertexAttribArray(i);
		const GLenum Type = VertexAttributeTypeToOpenGL(Attr.m_Type);
		const void *pOffset = reinterpret_cast<const void *>(Attr.m_Offset);
		if(Attr.m_Mode == IGraphics::EVertexAttributeMode::FLOAT)
			glVertexAttribPointer(i, Attr.m_ComponentCount, Type, (GLboolean)Attr.m_Normalized, static_cast<GLsizei>(Desc.m_Stride), pOffset);
		else
			glVertexAttribIPointer(i, Attr.m_ComponentCount, Type, static_cast<GLsizei>(Desc.m_Stride), pOffset);
	}
	return &Array;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	const int Index = pCommand->m_Buffer.Id();
	// create necessary space
	if((size_t)Index >= m_vBufferObjectIndices.size())
	{
		m_vBufferObjectIndices.resize(Index + 1, 0);
		m_vBufferObjectTargets.resize(Index + 1, 0);
	}

	GLuint VertBufferId = 0;
	const GLenum Target = BufferInitTarget(pCommand->m_Desc.m_Usage);

	glGenBuffers(1, &VertBufferId);
	BindBufferObject(Target, VertBufferId);
	glBufferData(Target, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);

	m_vBufferObjectIndices[Index] = VertBufferId;
	m_vBufferObjectTargets[Index] = Target;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	int Index = pCommand->m_Buffer.Id();

	const GLenum Target = m_vBufferObjectTargets[Index];
	BindBufferObject(Target, m_vBufferObjectIndices[Index]);
	glBufferData(Target, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
{
	int Index = pCommand->m_Buffer.Id();

	DestroyVertexArray(Index);
	glDeleteBuffers(1, &m_vBufferObjectIndices[Index]);
	m_vBufferObjectIndices[Index] = 0;
	m_vBufferObjectTargets[Index] = 0;
	std::fill(std::begin(m_aLastIndexBufferBound), std::end(m_aLastIndexBufferBound), 0);
	for(auto &Array : m_vVertexArrays)
		Array.m_LastIndexBufferBound = 0;
}

void CCommandProcessorFragment_OpenGL3_3::RenderDualAtlasComposite(const CCommandBuffer::SState &State, uint32_t IndexCount, size_t IndexOffset, int AtlasTextureIndex, const CCommandBuffer::SDrawDataDualAtlas &DrawData)
{
	UseProgram(m_pTextProgram);

	constexpr int Slot = 0;
	glBindTexture(GL_TEXTURE_2D, m_vTextures[AtlasTextureIndex].m_Tex);
	glBindSampler(Slot, m_vTextures[AtlasTextureIndex].m_Sampler);

	if(m_pTextProgram->m_LastTextSampler != Slot)
	{
		m_pTextProgram->SetUniform(m_pTextProgram->m_LocTextSampler, Slot);
		m_pTextProgram->m_LastTextSampler = Slot;
	}

	SetState(State, m_pTextProgram);

	if(m_pTextProgram->m_LastTextureSize != DrawData.m_TextureSize)
	{
		m_pTextProgram->SetUniform(m_pTextProgram->m_LocTextureSize, DrawData.m_TextureSize);
		m_pTextProgram->m_LastTextureSize = DrawData.m_TextureSize;
	}

	if(m_pTextProgram->m_LastOutlineColor != DrawData.m_SecondaryColor)
	{
		m_pTextProgram->SetUniformVec4(m_pTextProgram->m_LocOutlineColor, 1, (float *)&DrawData.m_SecondaryColor);
		m_pTextProgram->m_LastOutlineColor = DrawData.m_SecondaryColor;
	}

	if(m_pTextProgram->m_LastColor != DrawData.m_PrimaryColor)
	{
		m_pTextProgram->SetUniformVec4(m_pTextProgram->m_LocColor, 1, (float *)&DrawData.m_PrimaryColor);
		m_pTextProgram->m_LastColor = DrawData.m_PrimaryColor;
	}

	glDrawElements(GL_TRIANGLES, IndexCount, GL_UNSIGNED_INT, reinterpret_cast<const void *>(IndexOffset));
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	// CGraphics_Threaded::CheckIndexedDraw has already rejected everything this
	// would catch; the assert is here so a new producer notices at once.
	dbg_assert(IsIndexedDrawConsistent(*pCommand), "Backend received an inconsistent indexed draw");
	const EPipelineProgram Program = pCommand->m_Program;
	if(pCommand->m_IndexCount == 0)
	{
		return; // nothing to draw
	}

	SVertexArray *pArray = VertexArrayFor(pCommand->m_VertexBuffer, pCommand->m_Layout);
	if(pArray == nullptr)
		return;

	const TWGLuint IndexBuffer = m_vBufferObjectIndices[pCommand->m_IndexBuffer.Id()];
	glBindVertexArray(pArray->m_VertArrayId);
	if(pArray->m_LastIndexBufferBound != IndexBuffer)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);
		pArray->m_LastIndexBufferBound = IndexBuffer;
	}

	if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
	{
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
		if(pDrawData == nullptr)
			return;
		RenderDualAtlasComposite(pCommand->m_State, pCommand->m_IndexCount, pCommand->m_IndexOffset, pCommand->m_State.m_Texture.Id(), *pDrawData);
		return;
	}
	else if(Program == EPipelineProgram::ARRAY_COLOR || Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM)
	{
		const bool HasTransform = Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM;
		const auto *pColorData = HasTransform ? nullptr : pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColor>();
		const auto *pTransformData = HasTransform ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColorTransform>() : nullptr;
		if((HasTransform && pTransformData == nullptr) || (!HasTransform && pColorData == nullptr))
			return;

		CGLSLTileProgram *pProgram;
		if(HasTransform)
			pProgram = IsTexturedState(pCommand->m_State) ? m_pBorderTileProgramTextured : m_pBorderTileProgram;
		else
			pProgram = IsTexturedState(pCommand->m_State) ? m_pTileProgramTextured : m_pTileProgram;
		UseProgram(pProgram);
		SetState(pCommand->m_State, pProgram, true);
		const ColorRGBA &Color = HasTransform ? pTransformData->m_Color : pColorData->m_Color;
		pProgram->SetUniformVec4(pProgram->m_LocColor, 1, (float *)&Color);
		if(HasTransform)
		{
			pProgram->SetUniformVec2(pProgram->m_LocOffset, 1, (float *)&pTransformData->m_Offset);
			pProgram->SetUniformVec2(pProgram->m_LocScale, 1, (float *)&pTransformData->m_Scale);
		}
		glDrawElements(GL_TRIANGLES, pCommand->m_IndexCount, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset));
		return;
	}
	else if(Program == EPipelineProgram::QUAD_PER_ITEM || Program == EPipelineProgram::QUAD_SHARED)
	{
		const bool Grouped = Program == EPipelineProgram::QUAD_SHARED;
		const uint32_t QuadCount = pCommand->m_IndexCount / 6;
		const auto *pQuadData = Grouped ?
						pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataQuadTransform>() :
						pCommand->m_ArrayData.Get<CCommandBuffer::SDrawDataQuadTransform>(QuadCount);
		if(pQuadData == nullptr)
			return;

		CGLSLQuadProgram *pProgram;
		if(Grouped)
			pProgram = IsTexturedState(pCommand->m_State) ? m_pQuadProgramTexturedGrouped : m_pQuadProgramGrouped;
		else
			pProgram = IsTexturedState(pCommand->m_State) ? m_pQuadProgramTextured : m_pQuadProgram;
		UseProgram(pProgram);
		SetState(pCommand->m_State, pProgram);

		if(Grouped)
		{
			pProgram->SetUniformVec4(pProgram->m_LocColors, 1, (float *)&pQuadData->m_Color);
			pProgram->SetUniformVec2(pProgram->m_LocOffsets, 1, (float *)&pQuadData->m_Offset);
			pProgram->SetUniform(pProgram->m_LocRotations, 1, &pQuadData->m_Rotation);
			glDrawElements(GL_TRIANGLES, pCommand->m_IndexCount, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset));
			return;
		}

		ColorRGBA aColors[ms_MaxQuadsPossible];
		vec2 aOffsets[ms_MaxQuadsPossible];
		float aRotations[ms_MaxQuadsPossible];
		uint32_t QuadsLeft = QuadCount;
		size_t QuadOffset = 0;
		const size_t BaseQuadOffset = pCommand->m_IndexOffset / (6 * sizeof(uint32_t));
		while(QuadsLeft > 0)
		{
			const int ActualQuadCount = std::min<uint32_t>(QuadsLeft, m_MaxQuadsAtOnce);
			for(int i = 0; i < ActualQuadCount; ++i)
			{
				aColors[i] = pQuadData[QuadOffset + i].m_Color;
				aOffsets[i] = pQuadData[QuadOffset + i].m_Offset;
				aRotations[i] = pQuadData[QuadOffset + i].m_Rotation;
			}
			pProgram->SetUniformVec4(pProgram->m_LocColors, ActualQuadCount, (float *)aColors);
			pProgram->SetUniformVec2(pProgram->m_LocOffsets, ActualQuadCount, (float *)aOffsets);
			pProgram->SetUniform(pProgram->m_LocRotations, ActualQuadCount, aRotations);
			pProgram->SetUniform(pProgram->m_LocQuadOffset, static_cast<int>(BaseQuadOffset + QuadOffset));
			glDrawElements(GL_TRIANGLES, ActualQuadCount * 6, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset + QuadOffset * (6 * sizeof(uint32_t))));

			QuadsLeft -= ActualQuadCount;
			QuadOffset += ActualQuadCount;
		}
		return;
	}
	else if(Program == EPipelineProgram::PRIMITIVE)
	{
		CGLSLTWProgram *pProgram = m_pPrimitiveProgram;
		if(IsTexturedState(pCommand->m_State))
			pProgram = m_pPrimitiveProgramTextured;
		UseProgram(pProgram);
		SetState(pCommand->m_State, pProgram);
	}
	else if(Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR)
	{
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>();
		if(pDrawData == nullptr)
			return;

		CGLSLPrimitiveExProgram *pProgram = m_pPrimitiveExProgram;
		if(IsTexturedState(pCommand->m_State))
			pProgram = m_pPrimitiveExProgramTextured;

		UseProgram(pProgram);
		SetState(pCommand->m_State, pProgram);

		if(pDrawData->m_Rotation != 0.0f && pProgram->m_LastCenter != pDrawData->m_RotationCenter)
		{
			pProgram->SetUniformVec2(pProgram->m_LocCenter, 1, (float *)&pDrawData->m_RotationCenter);
			pProgram->m_LastCenter = pDrawData->m_RotationCenter;
		}

		if(pProgram->m_LastRotation != pDrawData->m_Rotation)
		{
			pProgram->SetUniform(pProgram->m_LocRotation, pDrawData->m_Rotation);
			pProgram->m_LastRotation = pDrawData->m_Rotation;
		}

		if(pProgram->m_LastVerticesColor != pDrawData->m_Color)
		{
			pProgram->SetUniformVec4(pProgram->m_LocVertciesColor, 1, (float *)&pDrawData->m_Color);
			pProgram->m_LastVerticesColor = pDrawData->m_Color;
		}
	}
	else if(Program == EPipelineProgram::PRIMITIVE_INSTANCED)
	{
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveInstanced>();
		const auto *pInstanceData = pCommand->m_ArrayData.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(pCommand->m_InstanceCount);
		if(pDrawData == nullptr || pInstanceData == nullptr || pCommand->m_InstanceCount == 0)
			return;

		UseProgram(m_pSpriteProgramMultiple);
		SetState(pCommand->m_State, m_pSpriteProgramMultiple);

		if(m_pSpriteProgramMultiple->m_LastCenter != pDrawData->m_RotationCenter)
		{
			m_pSpriteProgramMultiple->SetUniformVec2(m_pSpriteProgramMultiple->m_LocCenter, 1, (float *)&pDrawData->m_RotationCenter);
			m_pSpriteProgramMultiple->m_LastCenter = pDrawData->m_RotationCenter;
		}

		if(m_pSpriteProgramMultiple->m_LastVerticesColor != pDrawData->m_Color)
		{
			m_pSpriteProgramMultiple->SetUniformVec4(m_pSpriteProgramMultiple->m_LocVertciesColor, 1, (float *)&pDrawData->m_Color);
			m_pSpriteProgramMultiple->m_LastVerticesColor = pDrawData->m_Color;
		}

		uint32_t RemainingInstances = pCommand->m_InstanceCount;
		size_t InstanceOffset = 0;

		// 4 for the center (always use vec4), 16 for the matrix and 8 for sampler/color.
		constexpr uint32_t MaxInstancesPerDraw = 256 - 4 - 16 - 8;
		while(RemainingInstances > 0)
		{
			const uint32_t InstanceCount = std::min(RemainingInstances, MaxInstancesPerDraw);
			m_pSpriteProgramMultiple->SetUniformVec4(m_pSpriteProgramMultiple->m_LocRSP, InstanceCount, reinterpret_cast<const float *>(pInstanceData + InstanceOffset));
			glDrawElementsInstanced(GL_TRIANGLES, pCommand->m_IndexCount, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset), InstanceCount);

			InstanceOffset += InstanceCount;
			RemainingInstances -= InstanceCount;
		}
		return;
	}
	else
	{
		return;
	}

	glDrawElements(GL_TRIANGLES, pCommand->m_IndexCount, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset));
}

// ------------ CCommandProcessorFragment_OpenGLShared

void CCommandProcessorFragment_OpenGLShared::UseProgram(CGLSLTWProgram *pProgram)
{
	pProgram->UseProgram();
}

void CCommandProcessorFragment_OpenGLShared::SetState(const CCommandBuffer::SState &State, CGLSLTWProgram *pProgram, bool Use2DArrayTextures)
{
	if(State.m_BlendMode != m_LastBlendMode)
	{
		switch(State.m_BlendMode)
		{
		case EBlendMode::NONE:
			glDisable(GL_BLEND);
			break;
		case EBlendMode::ALPHA:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case EBlendMode::ADDITIVE:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		default:
			dbg_assert_failed("Invalid blend mode: %d", (int)State.m_BlendMode);
		};

		m_LastBlendMode = State.m_BlendMode;
	}

	SetScissor(State);

	if(!IsNewApi())
	{
		glDisable(GL_TEXTURE_2D);
		if(!m_HasShaders)
		{
			if(m_Has3DTextures)
				glDisable(GL_TEXTURE_3D);
			if(m_Has2DArrayTextures)
			{
				glDisable(m_2DArrayTarget);
			}
		}
	}

	// texture
	if(IsTexturedState(State))
	{
		int Slot = 0;
		if(!Use2DArrayTextures)
		{
			if(!IsNewApi() && !m_HasShaders)
				glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, m_vTextures[State.m_Texture.Id()].m_Tex);
			if(IsNewApi())
				glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler);
		}
		else
		{
			if(!m_Has2DArrayTextures)
			{
				if(!IsNewApi() && !m_HasShaders)
					glEnable(GL_TEXTURE_3D);
				glBindTexture(GL_TEXTURE_3D, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
				if(IsNewApi())
					glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler2DArray);
			}
			else
			{
				if(!IsNewApi() && !m_HasShaders)
					glEnable(m_2DArrayTarget);
				glBindTexture(m_2DArrayTarget, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
				if(IsNewApi())
					glBindSampler(Slot, m_vTextures[State.m_Texture.Id()].m_Sampler2DArray);
			}
		}

		if(pProgram->m_LastTextureSampler != Slot)
		{
			pProgram->SetUniform(pProgram->m_LocTextureSampler, Slot);
			pProgram->m_LastTextureSampler = Slot;
		}

		if(m_vTextures[State.m_Texture.Id()].m_LastWrapMode != State.m_WrapMode && !Use2DArrayTextures)
		{
			switch(State.m_WrapMode)
			{
			case EWrapMode::REPEAT:
				if(IsNewApi())
				{
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
				}
				break;
			case EWrapMode::CLAMP:
				if(IsNewApi())
				{
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glSamplerParameteri(m_vTextures[State.m_Texture.Id()].m_Sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				}
				break;
			default:
				dbg_assert_failed("Invalid wrap mode: %d", (int)State.m_WrapMode);
			};
			m_vTextures[State.m_Texture.Id()].m_LastWrapMode = State.m_WrapMode;
		}
	}

	CCommandBuffer::SPoint ScreenTL = State.m_ScreenTL;
	CCommandBuffer::SPoint ScreenBR = State.m_ScreenBR;
	if(m_RenderingToTexture)
		std::swap(ScreenTL.y, ScreenBR.y);
	if(pProgram->m_LastScreenTL != ScreenTL || pProgram->m_LastScreenBR != ScreenBR)
	{
		pProgram->m_LastScreenTL = ScreenTL;
		pProgram->m_LastScreenBR = ScreenBR;

		// screen mapping
		// orthographic projection matrix
		// the z coordinate is the same for every vertex, so just ignore the z coordinate and set it in the shaders
		vec2 Scale, Translate;
		// OpenGL's clip space has y pointing up.
		if(!ScreenToClip(ScreenTL, ScreenBR, true, Scale, Translate))
			return;
		float m[2 * 4] = {
			Scale.x,
			0,
			0,
			Translate.x,
			0,
			Scale.y,
			0,
			Translate.y,
		};

		// transpose bcs of column-major order of opengl
		glUniformMatrix4x2fv(pProgram->m_LocPos, 1, true, (float *)&m);
	}
}

#endif
