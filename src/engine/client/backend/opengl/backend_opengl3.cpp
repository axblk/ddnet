#include "backend_opengl3.h"

#include <base/detect.h>
#include <base/log.h>

#if defined(BACKEND_AS_OPENGL_ES) || !defined(CONF_BACKEND_OPENGL_ES)

#ifndef BACKEND_AS_OPENGL_ES
#include <GL/glew.h>
#else
#include <GLES3/gl3.h>
#endif

#include <engine/client/backend/glsl_shader_compiler.h>
#include <engine/client/backend/opengl/opengl_sl.h>
#include <engine/client/backend/opengl/opengl_sl_program.h>
#include <engine/client/backend_sdl.h>
#include <engine/gfx/image_manipulation.h>

#include <algorithm>
#include <memory>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// WebGL2 defines the type of a buffer at the first bind to a buffer target
// this is different to GLES 3 (https://www.khronos.org/registry/webgl/specs/latest/2.0/#5.1)
static constexpr GLenum BUFFER_INIT_VERTEX_TARGET = GL_ARRAY_BUFFER;
#else
static constexpr GLenum BUFFER_INIT_VERTEX_TARGET = GL_COPY_WRITE_BUFFER;
#endif

static GLenum VertexAttributeTypeToOpenGL(IGraphics::EVertexAttributeType Type)
{
	switch(Type)
	{
	case IGraphics::EVertexAttributeType::FLOAT32: return GL_FLOAT;
	case IGraphics::EVertexAttributeType::UINT8: return GL_UNSIGNED_BYTE;
	case IGraphics::EVertexAttributeType::UINT16: return GL_UNSIGNED_SHORT;
	case IGraphics::EVertexAttributeType::INT32: return GL_INT;
	case IGraphics::EVertexAttributeType::UINT32: return GL_UNSIGNED_INT;
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

void CCommandProcessorFragment_OpenGL3_3::InitPrimExProgram(CGLSLPrimitiveExProgram *pProgram, CGLSLCompiler *pCompiler, IStorage *pStorage, bool Textured)
{
	CGLSL PrimitiveVertexShader;
	CGLSL PrimitiveFragmentShader;
	if(Textured)
		pCompiler->AddDefine("TW_TEXTURED", "");
	PrimitiveVertexShader.LoadShader(pCompiler, pStorage, "shader/primex.vert", GL_VERTEX_SHADER);
	PrimitiveFragmentShader.LoadShader(pCompiler, pStorage, "shader/primex.frag", GL_FRAGMENT_SHADER);
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
	m_pBlurProgram = new CGLSLPrimitiveProgram;
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
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/prim.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/prim.frag", GL_FRAGMENT_SHADER);

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
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/prim.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/prim.frag", GL_FRAGMENT_SHADER);
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
		CGLSL BlurFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/prim.vert", GL_VERTEX_SHADER);
		BlurFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/blur.frag", GL_FRAGMENT_SHADER);

		m_pBlurProgram->CreateProgram();
		m_pBlurProgram->AddShader(&PrimitiveVertexShader);
		m_pBlurProgram->AddShader(&BlurFragmentShader);
		m_pBlurProgram->LinkProgram();

		UseProgram(m_pBlurProgram);

		m_pBlurProgram->m_LocPos = m_pBlurProgram->GetUniformLoc("gPos");
		m_pBlurProgram->m_LocTextureSampler = m_pBlurProgram->GetUniformLoc("gTextureSampler");
	}

	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		ShaderCompiler.AddDefine("TW_MODERN_GL", "");
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.frag", GL_FRAGMENT_SHADER);
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
		ShaderCompiler.AddDefine("TW_MODERN_GL", "");
		ShaderCompiler.AddDefine("TW_TEXTURED", "");
		if(!pCommand->m_pCapabilities->m_2DArrayTextures)
			ShaderCompiler.AddDefine("TW_3D_TEXTURED", "");
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/pipeline.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.frag", GL_FRAGMENT_SHADER);

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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/tile_border.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/quad.frag", GL_FRAGMENT_SHADER);
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
		VertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/text.vert", GL_VERTEX_SHADER);
		FragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/text.frag", GL_FRAGMENT_SHADER);

		m_pTextProgram->CreateProgram();
		m_pTextProgram->AddShader(&VertexShader);
		m_pTextProgram->AddShader(&FragmentShader);
		m_pTextProgram->LinkProgram();

		UseProgram(m_pTextProgram);

		m_pTextProgram->m_LocPos = m_pTextProgram->GetUniformLoc("gPos");
		m_pTextProgram->m_LocTextureSampler = -1;
		m_pTextProgram->m_LocTextSampler = m_pTextProgram->GetUniformLoc("gTextSampler");
		m_pTextProgram->m_LocTextOutlineSampler = m_pTextProgram->GetUniformLoc("gTextOutlineSampler");
		m_pTextProgram->m_LocColor = m_pTextProgram->GetUniformLoc("gVertColor");
		m_pTextProgram->m_LocOutlineColor = m_pTextProgram->GetUniformLoc("gVertOutlineColor");
		m_pTextProgram->m_LocTextureSize = m_pTextProgram->GetUniformLoc("gTextureSize");
	}
	InitPrimExProgram(m_pPrimitiveExProgram, &ShaderCompiler, pCommand->m_pStorage, false);
	InitPrimExProgram(m_pPrimitiveExProgramTextured, &ShaderCompiler, pCommand->m_pStorage, true);
	{
		CGLSL PrimitiveVertexShader;
		CGLSL PrimitiveFragmentShader;
		PrimitiveVertexShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/spritemulti.vert", GL_VERTEX_SHADER);
		PrimitiveFragmentShader.LoadShader(&ShaderCompiler, pCommand->m_pStorage, "shader/spritemulti.frag", GL_FRAGMENT_SHADER);

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
	glUseProgram(0);

	// clean up everything
	delete m_pPrimitiveProgram;
	delete m_pPrimitiveProgramTextured;
	delete m_pBlurProgram;
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

	for(size_t i = 0; i < m_vBufferContainers.size(); ++i)
	{
		DestroyBufferContainer(i);
	}

	m_vBufferContainers.clear();
}

void CCommandProcessorFragment_OpenGL3_3::TextureUpdate(int Slot, int X, int Y, int Width, int Height, int GLFormat, uint8_t *pTexData)
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

		pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, GLFormatToPixelSize(GLFormat)));
		pTexData = pOwnedTexData.get();
	}

	glTexSubImage2D(GL_TEXTURE_2D, 0, X, Y, Width, Height, GLFormat, GL_UNSIGNED_BYTE, pTexData);
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
	const int GLFormat = Desc.m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? GL_RGBA : GL_RED;
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

			pOwnedTexData.reset(ResizeImage(pTexData, OldWidth, OldHeight, Width, Height, GLFormatToPixelSize(GLFormat)));
			pTexData = pOwnedTexData.get();
		}
	}
	m_vTextures[Slot].m_Width = Width;
	m_vTextures[Slot].m_Height = Height;
	m_vTextures[Slot].m_RescaleCount = RescaleCount;

	if(GLStoreFormat == GL_RED)
		GLStoreFormat = GL_R8;
	const size_t PixelSize = GLFormatToPixelSize(GLFormat);

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

		if(Desc.m_Layering == IGraphics::ETextureLayering::ARRAY_2D)
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

			int ConvertWidth = Width;
			int ConvertHeight = Height;

			if(ConvertWidth == 0 || (ConvertWidth % LayerColumns) != 0 || ConvertHeight == 0 || (ConvertHeight % LayerRows) != 0)
			{
				int NewWidth = std::max(HighestBit(ConvertWidth / LayerColumns), 1) * LayerColumns;
				int NewHeight = std::max(HighestBit(ConvertHeight / LayerRows), 1) * LayerRows;
				pOwnedTexData.reset(ResizeImage(pTexData, ConvertWidth, ConvertHeight, NewWidth, NewHeight, GLFormatToPixelSize(GLFormat)));
				log_debug("gfx/opengl", "3D/2D array texture was resized. Slot=%d Size=(%d, %d) Resized=(%d, %d)", Slot, ConvertWidth, ConvertHeight, NewWidth, NewHeight);

				ConvertWidth = NewWidth;
				ConvertHeight = NewHeight;

				pTexData = pOwnedTexData.get();
			}

			int Image3DWidth, Image3DHeight;
			std::unique_ptr<uint8_t, decltype(&free)> pImageData3D(static_cast<uint8_t *>(malloc((size_t)PixelSize * ConvertWidth * ConvertHeight)), free);
			dbg_assert(pImageData3D != nullptr, "Failed to allocate 2D array texture conversion memory");
			Texture2DTo3D(pTexData, ConvertWidth, ConvertHeight, PixelSize, LayerColumns, LayerRows, pImageData3D.get(), Image3DWidth, Image3DHeight);
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

	const int GLFormat = pCommand->m_Format == IGraphics::ETextureFormat::RGBA8_UNORM ? GL_RGBA : GL_RED;
	TextureUpdate(pCommand->m_Texture.Id(), static_cast<int>(pCommand->m_Region.m_X), static_cast<int>(pCommand->m_Region.m_Y), static_cast<int>(pCommand->m_Region.m_Width), static_cast<int>(pCommand->m_Region.m_Height), GLFormat, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
	const CTexture &Texture = m_vTextures[pCommand->m_Texture.Id()];
	if(Texture.m_Framebuffer == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COLOR_TARGET) == 0 || (Texture.m_Usage & IGraphics::TEXTURE_USAGE_COPY_SOURCE) == 0)
		return;

	CImageInfo &Image = pCommand->m_pResult->m_Image;
	Image.m_Width = Texture.m_Width;
	Image.m_Height = Texture.m_Height;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	Image.Allocate();
	if(Image.m_pData == nullptr)
		return;

	GLint PreviousFramebuffer;
	GLint PackAlignment;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &PreviousFramebuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &PackAlignment);
	glBindFramebuffer(GL_FRAMEBUFFER, Texture.m_Framebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, Texture.m_Width, Texture.m_Height, GL_RGBA, GL_UNSIGNED_BYTE, Image.m_pData);
	glPixelStorei(GL_PACK_ALIGNMENT, PackAlignment);
	glBindFramebuffer(GL_FRAMEBUFFER, PreviousFramebuffer);
	const size_t RowSize = Image.m_Width * 4;
	for(size_t Y = 0; Y < Image.m_Height / 2; ++Y)
	{
		uint8_t *pTop = Image.m_pData + Y * RowSize;
		uint8_t *pBottom = Image.m_pData + (Image.m_Height - Y - 1) * RowSize;
		std::swap_ranges(pTop, pTop + RowSize, pBottom);
	}
	pCommand->m_pResult->m_Ok = true;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
{
	// if clip is still active, force disable it for clearing, enable it again afterwards
	bool ClipWasEnabled = m_LastClipEnable;
	if(ClipWasEnabled)
	{
		glDisable(GL_SCISSOR_TEST);
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
	const EPipelineProgram Program = PipelineProgram(pCommand->m_Pipeline);
	const bool TextureArray = Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY;
	const bool Blur = Program == EPipelineProgram::BLUR;
	if(!TextureArray && Program != EPipelineProgram::PRIMITIVE && !Blur)
		return;
	const uint32_t VerticesPerPrim = VerticesPerPrimitive(pCommand->m_PrimitiveType);
	if(VerticesPerPrim == 0 || pCommand->m_VertexCount == 0 || pCommand->m_VertexCount % VerticesPerPrim != 0)
		return;

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
	if(Blur && (!Textured || pCommand->m_State.m_BlendMode != EBlendMode::NONE))
		return;
	CGLSLTWProgram *pProgram = Blur ? static_cast<CGLSLTWProgram *>(m_pBlurProgram) : (TextureArray ? static_cast<CGLSLTWProgram *>(m_pPrimitive3DProgram) : m_pPrimitiveProgram);
	if(Textured && !Blur)
		pProgram = TextureArray ? static_cast<CGLSLTWProgram *>(m_pPrimitive3DProgramTextured) : m_pPrimitiveProgramTextured;
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

void CCommandProcessorFragment_OpenGL3_3::DestroyBufferContainer(int Index, bool DeleteBOs)
{
	SBufferContainer &BufferContainer = m_vBufferContainers[Index];
	if(BufferContainer.m_VertArrayId != 0)
		glDeleteVertexArrays(1, &BufferContainer.m_VertArrayId);

	// all buffer objects can deleted automatically, so the program doesn't need to deal with them (e.g. causing crashes because of driver bugs)
	if(DeleteBOs)
	{
		int VertBufferId = BufferContainer.m_ContainerInfo.m_VertBufferBinding.Id();
		if(VertBufferId != -1)
		{
			glDeleteBuffers(1, &m_vBufferObjectIndices[VertBufferId]);
		}
	}

	BufferContainer.m_LastIndexBufferBound = 0;
	BufferContainer.m_ContainerInfo.m_vAttributes.clear();
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	const int Index = pCommand->m_Buffer.Id();
	// create necessary space
	if((size_t)Index >= m_vBufferObjectIndices.size())
	{
		m_vBufferObjectIndices.resize(Index + 1, 0);
	}

	GLuint VertBufferId = 0;

	glGenBuffers(1, &VertBufferId);
	glBindBuffer(BUFFER_INIT_VERTEX_TARGET, VertBufferId);
	glBufferData(BUFFER_INIT_VERTEX_TARGET, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);

	m_vBufferObjectIndices[Index] = VertBufferId;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	int Index = pCommand->m_Buffer.Id();

	glBindBuffer(BUFFER_INIT_VERTEX_TARGET, m_vBufferObjectIndices[Index]);
	glBufferData(BUFFER_INIT_VERTEX_TARGET, (GLsizeiptr)(pCommand->m_Desc.m_Size), pUploadData, GL_STATIC_DRAW);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_UpdateBufferObject(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand)
{
	void *pUploadData = pCommand->m_pUploadData;
	int Index = pCommand->m_Buffer.Id();

	glBindBuffer(BUFFER_INIT_VERTEX_TARGET, m_vBufferObjectIndices[Index]);
	glBufferSubData(BUFFER_INIT_VERTEX_TARGET, static_cast<GLintptr>(pCommand->m_Offset), (GLsizeiptr)(pCommand->m_DataSize), pUploadData);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_CopyBufferObject(const CCommandBuffer::SCommand_CopyBufferObject *pCommand)
{
	int WriteIndex = pCommand->m_WriteBuffer.Id();
	int ReadIndex = pCommand->m_ReadBuffer.Id();

	glBindBuffer(GL_COPY_WRITE_BUFFER, m_vBufferObjectIndices[WriteIndex]);
	glBindBuffer(GL_COPY_READ_BUFFER, m_vBufferObjectIndices[ReadIndex]);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLsizeiptr)(pCommand->m_ReadOffset), (GLsizeiptr)(pCommand->m_WriteOffset), (GLsizeiptr)pCommand->m_CopySize);
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
{
	int Index = pCommand->m_Buffer.Id();

	glDeleteBuffers(1, &m_vBufferObjectIndices[Index]);
	std::fill(std::begin(m_aLastIndexBufferBound), std::end(m_aLastIndexBufferBound), 0);
	for(auto &BufferContainer : m_vBufferContainers)
		BufferContainer.m_LastIndexBufferBound = 0;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand)
{
	const int Index = pCommand->m_BufferContainer.Id();
	// create necessary space
	if((size_t)Index >= m_vBufferContainers.size())
	{
		SBufferContainer Container;
		Container.m_ContainerInfo.m_Stride = 0;
		Container.m_ContainerInfo.m_VertBufferBinding.Invalidate();
		m_vBufferContainers.resize(Index + 1, Container);
	}

	SBufferContainer &BufferContainer = m_vBufferContainers[Index];
	glGenVertexArrays(1, &BufferContainer.m_VertArrayId);
	glBindVertexArray(BufferContainer.m_VertArrayId);

	BufferContainer.m_LastIndexBufferBound = 0;

	for(size_t i = 0; i < pCommand->m_AttrCount; ++i)
	{
		glEnableVertexAttribArray((GLuint)i);

		glBindBuffer(GL_ARRAY_BUFFER, m_vBufferObjectIndices[pCommand->m_VertBufferBinding.Id()]);

		const SBufferContainerInfo::SAttribute &Attr = pCommand->m_pAttributes[i];
		const GLenum Type = VertexAttributeTypeToOpenGL(Attr.m_Type);
		const void *pOffset = reinterpret_cast<const void *>(Attr.m_Offset);

		if(Attr.m_Mode == IGraphics::EVertexAttributeMode::FLOAT)
			glVertexAttribPointer((GLuint)i, Attr.m_ComponentCount, Type, (GLboolean)Attr.m_Normalized, static_cast<GLsizei>(pCommand->m_Stride), pOffset);
		else
			glVertexAttribIPointer((GLuint)i, Attr.m_ComponentCount, Type, static_cast<GLsizei>(pCommand->m_Stride), pOffset);

		BufferContainer.m_ContainerInfo.m_vAttributes.push_back(Attr);
	}

	BufferContainer.m_ContainerInfo.m_VertBufferBinding = pCommand->m_VertBufferBinding;
	BufferContainer.m_ContainerInfo.m_Stride = pCommand->m_Stride;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand)
{
	SBufferContainer &BufferContainer = m_vBufferContainers[pCommand->m_BufferContainer.Id()];

	glBindVertexArray(BufferContainer.m_VertArrayId);

	// disable all old attributes
	for(size_t i = 0; i < BufferContainer.m_ContainerInfo.m_vAttributes.size(); ++i)
	{
		glDisableVertexAttribArray((GLuint)i);
	}
	BufferContainer.m_ContainerInfo.m_vAttributes.clear();

	for(size_t i = 0; i < pCommand->m_AttrCount; ++i)
	{
		glEnableVertexAttribArray((GLuint)i);

		glBindBuffer(GL_ARRAY_BUFFER, m_vBufferObjectIndices[pCommand->m_VertBufferBinding.Id()]);
		const SBufferContainerInfo::SAttribute &Attr = pCommand->m_pAttributes[i];
		const GLenum Type = VertexAttributeTypeToOpenGL(Attr.m_Type);
		const void *pOffset = reinterpret_cast<const void *>(Attr.m_Offset);
		if(Attr.m_Mode == IGraphics::EVertexAttributeMode::FLOAT)
			glVertexAttribPointer((GLuint)i, Attr.m_ComponentCount, Type, Attr.m_Normalized, static_cast<GLsizei>(pCommand->m_Stride), pOffset);
		else
			glVertexAttribIPointer((GLuint)i, Attr.m_ComponentCount, Type, static_cast<GLsizei>(pCommand->m_Stride), pOffset);

		BufferContainer.m_ContainerInfo.m_vAttributes.push_back(Attr);
	}

	BufferContainer.m_ContainerInfo.m_VertBufferBinding = pCommand->m_VertBufferBinding;
	BufferContainer.m_ContainerInfo.m_Stride = pCommand->m_Stride;
}

void CCommandProcessorFragment_OpenGL3_3::Cmd_DeleteBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand)
{
	DestroyBufferContainer(pCommand->m_BufferContainer.Id(), pCommand->m_DestroyAllBO);
}

void CCommandProcessorFragment_OpenGL3_3::RenderDualAtlasComposite(const CCommandBuffer::SState &State, uint32_t IndexCount, size_t IndexOffset, int PrimaryTextureIndex, int SecondaryTextureIndex, const CCommandBuffer::SDrawDataDualAtlas &DrawData)
{
	UseProgram(m_pTextProgram);

	int SlotPrimary = 0;
	int SlotSecondary = 1;
	glBindTexture(GL_TEXTURE_2D, m_vTextures[PrimaryTextureIndex].m_Tex);
	glBindSampler(SlotPrimary, m_vTextures[PrimaryTextureIndex].m_Sampler);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_vTextures[SecondaryTextureIndex].m_Tex);
	glBindSampler(SlotSecondary, m_vTextures[SecondaryTextureIndex].m_Sampler);
	glActiveTexture(GL_TEXTURE0);

	if(m_pTextProgram->m_LastTextSampler != SlotPrimary)
	{
		m_pTextProgram->SetUniform(m_pTextProgram->m_LocTextSampler, SlotPrimary);
		m_pTextProgram->m_LastTextSampler = SlotPrimary;
	}

	if(m_pTextProgram->m_LastTextOutlineSampler != SlotSecondary)
	{
		m_pTextProgram->SetUniform(m_pTextProgram->m_LocTextOutlineSampler, SlotSecondary);
		m_pTextProgram->m_LastTextOutlineSampler = SlotSecondary;
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
	const EPipelineProgram Program = PipelineProgram(pCommand->m_Pipeline);
	if(pCommand->IsTransient())
	{
		const auto *pVertices = pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
		const auto *pRanges = pCommand->m_RangeData.Get<CCommandBuffer::SCommand_DrawIndexed::SIndexedDrawRange>(pCommand->m_RangeCount);
		const void *pIndices;
		GLenum IndexType;
		size_t IndexElementSize;
		if(pCommand->m_IndexType == IGraphics::EIndexType::UINT16)
		{
			pIndices = pCommand->m_IndexData.Get<uint16_t>(pCommand->m_IndexCount);
			IndexType = GL_UNSIGNED_SHORT;
			IndexElementSize = sizeof(uint16_t);
		}
		else
		{
			pIndices = pCommand->m_IndexData.Get<uint32_t>(pCommand->m_IndexCount);
			IndexType = GL_UNSIGNED_INT;
			IndexElementSize = sizeof(uint32_t);
		}
		const size_t VertexDataSize = pCommand->m_VertexData.m_Size;
		const size_t IndexDataSize = pCommand->m_IndexData.m_Size;
		const TWGLuint VertexBuffer = m_aPrimitiveDrawBufferId[m_LastStreamBuffer];
		const TWGLuint IndexBuffer = m_aPrimitiveDrawIndexBufferId[m_LastStreamBuffer];
		glBindVertexArray(m_aPrimitiveDrawVertexId[m_LastStreamBuffer]);
		glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, VertexDataSize, pVertices, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, IndexDataSize, pIndices, GL_STREAM_DRAW);
		m_aLastIndexBufferBound[m_LastStreamBuffer] = IndexBuffer;

		auto SetVertexOffset = [](size_t VertexOffset) {
			const size_t BaseOffset = VertexOffset * sizeof(CCommandBuffer::SVertex);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const void *>(BaseOffset));
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const void *>(BaseOffset + sizeof(float) * 2));
			glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(CCommandBuffer::SVertex), reinterpret_cast<const void *>(BaseOffset + sizeof(float) * 4));
		};
		for(uint32_t RangeIndex = 0; RangeIndex < pCommand->m_RangeCount; ++RangeIndex)
		{
			const auto &Range = pRanges[RangeIndex];
			CGLSLPrimitiveProgram *pProgram = IsTexturedState(Range.m_State) ? m_pPrimitiveProgramTextured : m_pPrimitiveProgram;
			UseProgram(pProgram);
			SetState(Range.m_State, pProgram);
			SetVertexOffset(Range.m_VertexOffset);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(Range.m_IndexCount), IndexType, reinterpret_cast<const void *>(static_cast<size_t>(Range.m_FirstIndex) * IndexElementSize));
		}
		SetVertexOffset(0);
		m_LastStreamBuffer = (m_LastStreamBuffer + 1 >= MAX_STREAM_BUFFER_COUNT ? 0 : m_LastStreamBuffer + 1);
		return;
	}
	if(pCommand->m_IndexCount == 0)
	{
		return; // nothing to draw
	}

	int Index = pCommand->m_BufferContainer.Id();
	// if space not there return
	if((size_t)Index >= m_vBufferContainers.size())
		return;

	SBufferContainer &BufferContainer = m_vBufferContainers[Index];
	if(BufferContainer.m_VertArrayId == 0)
		return;

	const TWGLuint IndexBuffer = m_vBufferObjectIndices[pCommand->m_IndexBuffer.Id()];
	glBindVertexArray(BufferContainer.m_VertArrayId);
	if(BufferContainer.m_LastIndexBufferBound != IndexBuffer)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);
		BufferContainer.m_LastIndexBufferBound = IndexBuffer;
	}

	if(Program == EPipelineProgram::DUAL_ATLAS_COMPOSITE)
	{
		constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
		const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
		if(!pCommand->m_TextureBinding.IsValid() || pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0 || pDrawData == nullptr)
			return;
		const auto &Binding = m_vTextureBindings[pCommand->m_TextureBinding.Id()];
		RenderDualAtlasComposite(pCommand->m_State, pCommand->m_IndexCount, pCommand->m_IndexOffset, Binding.m_aTextures[0].Id(), Binding.m_aTextures[1].Id(), *pDrawData);
		return;
	}
	else if(Program == EPipelineProgram::ARRAY_COLOR || Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM)
	{
		constexpr size_t TileIndexBytes = 6 * sizeof(uint32_t);
		if(pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % TileIndexBytes != 0)
			return;
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
		constexpr size_t QuadIndexBytes = 6 * sizeof(uint32_t);
		if(pCommand->m_IndexCount % 6 != 0 || pCommand->m_IndexOffset % QuadIndexBytes != 0)
			return;

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
		const size_t BaseQuadOffset = pCommand->m_IndexOffset / QuadIndexBytes;
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
			glDrawElements(GL_TRIANGLES, ActualQuadCount * 6, GL_UNSIGNED_INT, reinterpret_cast<const void *>(pCommand->m_IndexOffset + QuadOffset * QuadIndexBytes));

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

#endif
