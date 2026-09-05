#include "backend_opengl.h"

#include <base/dbg.h>
#include <base/detect.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client/backend_threaded.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#if !defined(CONF_BACKEND_OPENGL_ES)

#include <engine/gfx/image_manipulation.h>

#include <GL/glew.h>

// ------------ CCommandProcessorFragment_OpenGL
int CCommandProcessorFragment_OpenGL::ToGLFormat(IGraphics::ETextureFormat Format)
{
	// A two channel texture never gets here: the fixed function pipeline has no
	// two channel format, so the glyph atlas is split into two alpha textures.
	return Format == IGraphics::ETextureFormat::RGBA8_UNORM ? GL_RGBA : GL_ALPHA;
}

void CCommandProcessorFragment_OpenGL::SetState(const CCommandBuffer::SState &State, bool Use2DArrayTextures)
{
	// blend
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

	SetScissor(State);

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

	if(m_HasShaders && IsNewApi())
	{
		glBindSampler(0, 0);
	}

	// texture
	if(IsTexturedState(State))
	{
		if(!Use2DArrayTextures)
		{
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, m_vTextures[State.m_Texture.Id()].m_Tex);

			if(m_vTextures[State.m_Texture.Id()].m_LastWrapMode != State.m_WrapMode)
			{
				switch(State.m_WrapMode)
				{
				case EWrapMode::REPEAT:
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
					break;
				case EWrapMode::CLAMP:
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					break;
				default:
					dbg_assert_failed("Invalid wrap mode: %d", (int)State.m_WrapMode);
				};
				m_vTextures[State.m_Texture.Id()].m_LastWrapMode = State.m_WrapMode;
			}
		}
		else if(m_Has2DArrayTextures)
		{
			if(!m_HasShaders)
				glEnable(m_2DArrayTarget);
			glBindTexture(m_2DArrayTarget, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
		}
		else if(m_Has3DTextures)
		{
			if(!m_HasShaders)
				glEnable(GL_TEXTURE_3D);
			glBindTexture(GL_TEXTURE_3D, m_vTextures[State.m_Texture.Id()].m_Tex2DArray);
		}
		else
		{
			dbg_assert_failed("Should have either 2D, 3D or no texture array support");
		}
	}

	// screen mapping
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(State.m_ScreenTL.x, State.m_ScreenBR.x, State.m_ScreenBR.y, State.m_ScreenTL.y, -10.0f, 10.f);
}

bool CCommandProcessorFragment_OpenGL::Cmd_Init(const SCommand_Init *pCommand)
{
	if(!InitOpenGL(pCommand))
		return false;

	m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
	m_pTextureMemoryUsage->store(0, std::memory_order_relaxed);
	m_MaxTexSize = -1;

	m_OpenGLTextureLodBIAS = 0;

	m_Has2DArrayTextures = pCommand->m_pCapabilities->m_2DArrayTextures;
	m_2DArrayTarget = GL_TEXTURE_2D_ARRAY;

	m_LastBlendMode = EBlendMode::ALPHA;
	m_LastClipEnable = false;

	return true;
}

void CCommandProcessorFragment_OpenGL::TextureUpdate(int Slot, int X, int Y, int Width, int Height, IGraphics::ETextureFormat Format, uint8_t *pTexData)
{
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);

	if(!m_HasNPOTTextures)
	{
		float ResizeW = m_vTextures[Slot].m_ResizeWidth;
		float ResizeH = m_vTextures[Slot].m_ResizeHeight;
		if(ResizeW > 0 && ResizeH > 0)
		{
			int ResizedW = (int)(Width * ResizeW);
			int ResizedH = (int)(Height * ResizeH);

			pOwnedTexData.reset(ResizeImage(pTexData, Width, Height, ResizedW, ResizedH, IGraphics::PixelSize(Format)));
			pTexData = pOwnedTexData.get();

			Width = ResizedW;
			Height = ResizedH;
		}
	}

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

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
{
	DestroyTexture(pCommand->m_Texture.Id());
}

void CCommandProcessorFragment_OpenGL::CreateSplitChannelTexture(int Slot, const IGraphics::CTextureDesc &Desc, const uint8_t *pTexData)
{
	while(Slot >= (int)m_vTextures.size())
		m_vTextures.resize(m_vTextures.size() * 2);
	CTexture &Texture = m_vTextures[Slot];
	Texture.m_SourceWidth = Desc.m_Width;
	Texture.m_SourceHeight = Desc.m_Height;
	Texture.m_Width = Desc.m_Width;
	Texture.m_Height = Desc.m_Height;
	Texture.m_Format = Desc.m_Format;
	Texture.m_Usage = Desc.m_Usage;
	Texture.m_ResizeWidth = -1.f;
	Texture.m_ResizeHeight = -1.f;
	Texture.m_RescaleCount = 0;

	const size_t PixelCount = (size_t)Desc.m_Width * Desc.m_Height;
	std::vector<uint8_t> vChannel(pTexData != nullptr ? PixelCount : 0);
	TWGLuint *apNames[2] = {&Texture.m_Tex, &Texture.m_TexSecondChannel};
	for(int Channel = 0; Channel < 2; ++Channel)
	{
		if(pTexData != nullptr)
		{
			for(size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
				vChannel[Pixel] = pTexData[Pixel * 2 + Channel];
		}
		glGenTextures(1, apNames[Channel]);
		glBindTexture(GL_TEXTURE_2D, *apNames[Channel]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, Desc.m_Width, Desc.m_Height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, pTexData != nullptr ? vChannel.data() : nullptr);
	}
	Texture.m_MemSize = (int)(PixelCount * 2);
	m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) + Texture.m_MemSize, std::memory_order_relaxed);
}

void CCommandProcessorFragment_OpenGL::UpdateSplitChannelTexture(int Slot, int X, int Y, int Width, int Height, const uint8_t *pTexData)
{
	const size_t PixelCount = (size_t)Width * Height;
	std::vector<uint8_t> vChannel(PixelCount);
	const TWGLuint aNames[2] = {m_vTextures[Slot].m_Tex, m_vTextures[Slot].m_TexSecondChannel};
	for(int Channel = 0; Channel < 2; ++Channel)
	{
		for(size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
			vChannel[Pixel] = pTexData[Pixel * 2 + Channel];
		glBindTexture(GL_TEXTURE_2D, aNames[Channel]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, X, Y, Width, Height, GL_ALPHA, GL_UNSIGNED_BYTE, vChannel.data());
	}
}

void CCommandProcessorFragment_OpenGL::TextureCreate(int Slot, const IGraphics::CTextureDesc &Desc, uint8_t *pTexData)
{
	if(m_MaxTexSize == -1)
	{
		// Single byte rows have to be readable unpadded, and the split channel
		// path below uploads exactly those - so this cannot wait for the first
		// RGBA texture as it used to.
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_MaxTexSize);
	}
	if(Desc.m_Format == IGraphics::ETextureFormat::RG8_UNORM)
	{
		// The glyph atlas is the only two channel texture the client makes: flat,
		// power of two, no mipmaps, so splitting it needs none of what follows.
		dbg_assert(Desc.m_Mipmaps == IGraphics::ETextureMipmaps::NONE, "A split channel texture cannot be mipmapped");
		dbg_assert(Desc.m_Layering == IGraphics::ETextureLayering::NONE, "A split channel texture cannot be layered");
		CreateSplitChannelTexture(Slot, Desc, pTexData);
		return;
	}
	std::unique_ptr<uint8_t, decltype(&free)> pOwnedTexData(nullptr, free);
	int Width = Desc.m_Width;
	int Height = Desc.m_Height;
	const int GLFormat = ToGLFormat(Desc.m_Format);
	const int GLStoreFormat = GLFormat;

	while(Slot >= (int)m_vTextures.size())
		m_vTextures.resize(m_vTextures.size() * 2);
	m_vTextures[Slot].m_SourceWidth = Desc.m_Width;
	m_vTextures[Slot].m_SourceHeight = Desc.m_Height;
	m_vTextures[Slot].m_Format = Desc.m_Format;
	m_vTextures[Slot].m_Usage = Desc.m_Usage;

	m_vTextures[Slot].m_ResizeWidth = -1.f;
	m_vTextures[Slot].m_ResizeHeight = -1.f;

	if(!m_HasNPOTTextures)
	{
		int PowerOfTwoWidth = HighestBit(Width);
		int PowerOfTwoHeight = HighestBit(Height);
		if(Width != PowerOfTwoWidth || Height != PowerOfTwoHeight)
		{
			pOwnedTexData.reset(ResizeImage(pTexData, Width, Height, PowerOfTwoWidth, PowerOfTwoHeight, IGraphics::PixelSize(Desc.m_Format)));
			pTexData = pOwnedTexData.get();

			m_vTextures[Slot].m_ResizeWidth = (float)PowerOfTwoWidth / (float)Width;
			m_vTextures[Slot].m_ResizeHeight = (float)PowerOfTwoHeight / (float)Height;

			Width = PowerOfTwoWidth;
			Height = PowerOfTwoHeight;
		}
	}

	int RescaleCount = 0;
	if(GLFormat == GL_RGBA)
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

	const size_t PixelSize = IGraphics::PixelSize(Desc.m_Format);

	if(Desc.m_Create2D)
	{
		glGenTextures(1, &m_vTextures[Slot].m_Tex);
		glBindTexture(GL_TEXTURE_2D, m_vTextures[Slot].m_Tex);
	}

	if(Desc.m_Mipmaps == IGraphics::ETextureMipmaps::NONE || !m_HasMipMaps)
	{
		if(Desc.m_Create2D)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
		}
	}
	else
	{
		if(Desc.m_Create2D)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);

			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));

			glTexImage2D(GL_TEXTURE_2D, 0, GLStoreFormat, Width, Height, 0, GLFormat, GL_UNSIGNED_BYTE, pTexData);
		}

		if(Desc.m_Layering != IGraphics::ETextureLayering::NONE)
		{
			// Where there are no array textures the layers live in a volume; the
			// backend decides that, the caller only says that there are layers.
			const bool Is3DTexture = !m_Has2DArrayTextures;
			const int LayerColumns = Desc.m_LayerColumns;
			const int LayerRows = Desc.m_LayerRows;
			const int LayerCount = static_cast<int>(Desc.LayerCount());
			m_vTextures[Slot].m_LayerCount = LayerCount;

			glGenTextures(1, &m_vTextures[Slot].m_Tex2DArray);

			GLenum Target = GL_TEXTURE_3D;

			if(Is3DTexture)
			{
				Target = GL_TEXTURE_3D;
			}
			else
			{
				Target = m_2DArrayTarget;
			}

			glBindTexture(Target, m_vTextures[Slot].m_Tex2DArray);

			if(IsNewApi())
			{
				glGenSamplers(1, &m_vTextures[Slot].m_Sampler2DArray);
				glBindSampler(0, m_vTextures[Slot].m_Sampler2DArray);
			}

			glTexParameteri(Target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			if(Is3DTexture)
			{
				glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				if(IsNewApi())
					glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			}
			else
			{
				glTexParameteri(Target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(Target, GL_GENERATE_MIPMAP, GL_TRUE);
				if(IsNewApi())
					glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			}

			glTexParameteri(Target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(Target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(Target, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

			if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
				glTexParameterf(Target, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));

			if(IsNewApi())
			{
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glSamplerParameteri(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_WRAP_R, GL_MIRRORED_REPEAT);

				if(m_OpenGLTextureLodBIAS != 0 && !m_IsOpenGLES)
					glSamplerParameterf(m_vTextures[Slot].m_Sampler2DArray, GL_TEXTURE_LOD_BIAS, ((GLfloat)m_OpenGLTextureLodBIAS / 1000.0f));

				glBindSampler(0, 0);
			}

			int Image3DWidth, Image3DHeight;
			std::unique_ptr<uint8_t, decltype(&free)> pImageData3D = PrepareLayeredImage(pTexData, Width, Height, PixelSize, LayerColumns, LayerRows, Image3DWidth, Image3DHeight);
			dbg_assert(pImageData3D != nullptr, "Failed to allocate 2D array texture conversion memory");
			glTexImage3D(Target, 0, GLStoreFormat, Image3DWidth, Image3DHeight, LayerCount, 0, GLFormat, GL_UNSIGNED_BYTE, pImageData3D.get());
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

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
{
	TextureCreate(pCommand->m_Texture.Id(), pCommand->m_Desc, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
{
	if(!IsTextureUpdateValid(pCommand))
		return;
	if(pCommand->m_Format == IGraphics::ETextureFormat::RG8_UNORM)
	{
		UpdateSplitChannelTexture(pCommand->m_Texture.Id(), static_cast<int>(pCommand->m_Region.m_X), static_cast<int>(pCommand->m_Region.m_Y), static_cast<int>(pCommand->m_Region.m_Width), static_cast<int>(pCommand->m_Region.m_Height), pCommand->m_pData);
		return;
	}
	TextureUpdate(pCommand->m_Texture.Id(), static_cast<int>(pCommand->m_Region.m_X), static_cast<int>(pCommand->m_Region.m_Y), static_cast<int>(pCommand->m_Region.m_Width), static_cast<int>(pCommand->m_Region.m_Height), pCommand->m_Format, pCommand->m_pData);
}

void CCommandProcessorFragment_OpenGL::Cmd_Texture_Readback(const CCommandBuffer::SCommand_Texture_Readback *pCommand)
{
	pCommand->m_pResult->m_Ok = false;
}

void CCommandProcessorFragment_OpenGL::Cmd_Clear(const CCommandBuffer::SCommand_Clear *pCommand)
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
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glScissor(m_PresentationViewportX, m_PresentationViewportY, m_PresentationViewportWidth, m_PresentationViewportHeight);
		glEnable(GL_SCISSOR_TEST);
	}
	glClearColor(pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if(ClipWasEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
	}
	else if(ClearAroundCutout)
	{
		glDisable(GL_SCISSOR_TEST);
	}
}

// A quad is drawn as a quad where there is one, and as the two triangles the
// frontend's index buffer describes where there is not - OpenGL ES has no
// quads, and neither does a core profile.
void CCommandProcessorFragment_OpenGL::DrawPrimitives(EPrimitiveType PrimitiveType, uint32_t VertexCount, IGraphics::CBufferHandle IndexBuffer)
{
	switch(PrimitiveType)
	{
	case EPrimitiveType::QUADS:
	{
		glDrawArrays(GL_QUADS, 0, VertexCount);
		break;
	}
	case EPrimitiveType::TRIANGLES: glDrawArrays(GL_TRIANGLES, 0, VertexCount); break;
	case EPrimitiveType::LINES: glDrawArrays(GL_LINES, 0, VertexCount); break;
	default: dbg_assert_failed("Invalid primitive type: %d", (int)PrimitiveType);
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_Draw(const CCommandBuffer::SCommand_Draw *pCommand)
{
	const EPipelineProgram Program = pCommand->m_Program;
	const bool Layered = Program == EPipelineProgram::PRIMITIVE_TEXTURE_ARRAY;
	if(Program != EPipelineProgram::PRIMITIVE && !Layered)
	{
		DropCommand("a draw on a pipeline this OpenGL version has no program for");
		return;
	}
	const uint32_t VerticesPerPrim = VerticesPerPrimitive(pCommand->m_PrimitiveType);
	const auto *pVertices = Layered ? nullptr : pCommand->m_VertexData.Get<CCommandBuffer::SVertex>(pCommand->m_VertexCount);
	const auto *pLayeredVerticesConst = Layered ? pCommand->m_VertexData.Get<CCommandBuffer::SVertexTex3DStream>(pCommand->m_VertexCount) : nullptr;
	if((Layered ? pLayeredVerticesConst == nullptr : pVertices == nullptr) || VerticesPerPrim == 0 || pCommand->m_VertexCount == 0 || pCommand->m_VertexCount % VerticesPerPrim != 0)
	{
		DropCommand("a draw whose vertex data does not fit its primitive type");
		return;
	}
	if(Layered)
	{
		// The layer a vertex names is the third texture coordinate, which the
		// fixed function path can take as long as the texture is a volume - and
		// a volume is addressed in the middle of the slice, not by index.
		m_vExpandedLayeredVertices.assign(pLayeredVerticesConst, pLayeredVerticesConst + pCommand->m_VertexCount);
		for(auto &Vertex : m_vExpandedLayeredVertices)
			Vertex.m_Tex.w = LayerCoordinate(pCommand->m_State, Vertex.m_Tex.w);
		const auto *pLayeredVertices = m_vExpandedLayeredVertices.data();
		SetState(pCommand->m_State, true);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glVertexPointer(2, GL_FLOAT, sizeof(*pLayeredVertices), &pLayeredVertices->m_Pos);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*pLayeredVertices), &pLayeredVertices->m_Color);
		glTexCoordPointer(3, GL_FLOAT, sizeof(*pLayeredVertices), &pLayeredVertices->m_Tex);
		DrawPrimitives(pCommand->m_PrimitiveType, pCommand->m_VertexCount, pCommand->m_IndexBuffer);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		return;
	}
	SetState(pCommand->m_State);

	glVertexPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char *)pVertices);
	glTexCoordPointer(2, GL_FLOAT, sizeof(CCommandBuffer::SVertex), (char *)pVertices + sizeof(float) * 2);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CCommandBuffer::SVertex), (char *)pVertices + sizeof(float) * 4);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	DrawPrimitives(pCommand->m_PrimitiveType, pCommand->m_VertexCount, pCommand->m_IndexBuffer);
}

// ------------ buffer emulation for the fixed function path

void CCommandProcessorFragment_OpenGL::Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
{
	const size_t Index = pCommand->m_Buffer.Id();
	if(Index >= m_vEmulatedBuffers.size())
		m_vEmulatedBuffers.resize(Index + 1);
	SEmulatedBuffer &Buffer = m_vEmulatedBuffers[Index];
	Buffer.m_vData.assign(pCommand->m_Desc.m_Size, 0);
	if(pCommand->m_pUploadData != nullptr)
		mem_copy(Buffer.m_vData.data(), pCommand->m_pUploadData, pCommand->m_Desc.m_Size);
}

void CCommandProcessorFragment_OpenGL::Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
{
	const size_t Index = pCommand->m_Buffer.Id();
	if(Index >= m_vEmulatedBuffers.size())
		m_vEmulatedBuffers.resize(Index + 1);
	SEmulatedBuffer &Buffer = m_vEmulatedBuffers[Index];
	Buffer.m_vData.assign(pCommand->m_Desc.m_Size, 0);
	if(pCommand->m_pUploadData != nullptr)
		mem_copy(Buffer.m_vData.data(), pCommand->m_pUploadData, pCommand->m_Desc.m_Size);
	InvalidateContainersOfBuffer(Index);
}

void CCommandProcessorFragment_OpenGL::Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
{
	const size_t Index = pCommand->m_Buffer.Id();
	if(Index < m_vEmulatedBuffers.size())
		m_vEmulatedBuffers[Index].m_vData = {};
	InvalidateContainersOfBuffer(Index);
}

// ------------ converting a container into something fixed function can read

void CCommandProcessorFragment_OpenGL::ReleaseConvertedBuffer(SConvertedBuffer &Converted)
{
	if(Converted.m_BufferId != 0)
	{
		glDeleteBuffers(1, &Converted.m_BufferId);
		Converted.m_BufferId = 0;
	}
	Converted.m_vConverted.clear();
	Converted.m_vConverted.shrink_to_fit();
	Converted.m_vColors.clear();
	Converted.m_vColors.shrink_to_fit();
	Converted.m_VertexCount = 0;
	Converted.m_Form = EVertexForm::NONE;
}

void CCommandProcessorFragment_OpenGL::InvalidateContainersOfBuffer(size_t BufferIndex)
{
	if(BufferIndex < m_vConvertedBuffers.size())
		ReleaseConvertedBuffer(m_vConvertedBuffers[BufferIndex]);
}

CCommandProcessorFragment_OpenGL::SConvertedBuffer *CCommandProcessorFragment_OpenGL::ConvertedBufferFor(IGraphics::CBufferHandle Buffer, IGraphics::EVertexLayout Layout)
{
	if(!Buffer.IsValid() || Layout >= IGraphics::EVertexLayout::COUNT)
		return nullptr;
	const size_t BufferIndex = Buffer.Id();
	if(BufferIndex >= m_vEmulatedBuffers.size() || m_vEmulatedBuffers[BufferIndex].m_vData.empty())
		return nullptr;
	if(BufferIndex >= m_vConvertedBuffers.size())
		m_vConvertedBuffers.resize(BufferIndex + 1);
	SConvertedBuffer &Container = m_vConvertedBuffers[BufferIndex];
	if(Container.m_Layout == Layout && Container.m_VertexCount != 0)
		return &Container;
	ReleaseConvertedBuffer(Container);
	Container.m_Layout = Layout;
	Container.m_SourceBuffer = Buffer;

	const IGraphics::SVertexLayoutDesc &Info = IGraphics::VertexLayout(Layout);
	const std::vector<uint8_t> &vSource = m_vEmulatedBuffers[BufferIndex].m_vData;
	const size_t VertexCount = vSource.size() / Info.m_Stride;
	if(VertexCount == 0)
		return nullptr;

	// A tile carries its layer in the third texture coordinate and takes its
	// colour from the draw; everything else carries a colour and a plain pair
	// of texture coordinates.
	const bool Layered = Layout == IGraphics::EVertexLayout::TILE_TEXTURED;
	Container.m_Form = Layered ? EVertexForm::LAYERED : EVertexForm::PRIMITIVE;
	Container.m_VertexCount = VertexCount;

	const size_t ConvertedStride = Layered ? sizeof(CCommandBuffer::SVertexTex3DStream) : sizeof(CCommandBuffer::SVertex);
	Container.m_vConverted.resize(VertexCount * ConvertedStride);
	if(!Layered)
		Container.m_vColors.resize(VertexCount);

	for(size_t Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		const uint8_t *pSource = vSource.data() + Vertex * Info.m_Stride;
		const ColorRGBA Position = ReadAttribute(pSource, Info.m_aAttributes[0]);
		if(Layered)
		{
			const ColorRGBA TexCoord = ReadAttribute(pSource, Info.m_aAttributes[1]);
			CCommandBuffer::SVertexTex3DStream Converted;
			Converted.m_Pos = vec2(Position.r, Position.g);
			Converted.m_Color = CCommandBuffer::SColor(255, 255, 255, 255);
			// The layer stays the index it was named by. What it has to become
			// to address a volume is a scale on the texture matrix, set once
			// per draw rather than written into every vertex here.
			Converted.m_Tex = vec3(TexCoord.r, TexCoord.g, TexCoord.b);
			mem_copy(Container.m_vConverted.data() + Vertex * ConvertedStride, &Converted, sizeof(Converted));
		}
		else
		{
			const ColorRGBA Second = Info.m_AttributeCount >= 2 ? ReadAttribute(pSource, Info.m_aAttributes[1]) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
			const ColorRGBA Third = Info.m_AttributeCount >= 3 ? ReadAttribute(pSource, Info.m_aAttributes[2]) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
			// A quad layer puts the colour second and the texture coordinate
			// third; everything else has them the other way round.
			const bool ColourIsSecond = Info.m_aAttributes[1].m_Type == IGraphics::EVertexAttributeType::UINT8;
			const ColorRGBA TexCoord = ColourIsSecond ? Third : Second;
			const ColorRGBA Color = ColourIsSecond ? Second : Third;
			CCommandBuffer::SVertex Converted;
			Converted.m_Pos = vec2(Position.r, Position.g);
			Converted.m_Tex = vec2(TexCoord.r, TexCoord.g);
			Converted.m_Color = CCommandBuffer::SColor(
				(unsigned char)std::clamp(Color.r * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Color.g * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Color.b * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Color.a * 255.0f, 0.0f, 255.0f));
			Container.m_vColors[Vertex] = Converted.m_Color;
			mem_copy(Container.m_vConverted.data() + Vertex * ConvertedStride, &Converted, sizeof(Converted));
		}
	}

	if(m_HasBufferObjects)
	{
		glGenBuffers(1, &Container.m_BufferId);
		glBindBuffer(GL_ARRAY_BUFFER, Container.m_BufferId);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)Container.m_vConverted.size(), Container.m_vConverted.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		// The array lives on the device now, and the copy here would only be a
		// second one of the same thing.
		Container.m_vConverted.clear();
		Container.m_vConverted.shrink_to_fit();
	}
	return &Container;
}

void CCommandProcessorFragment_OpenGL::BindConvertedContainer(const SConvertedBuffer &Container, const CCommandBuffer::SColor *pColors)
{
	const uint8_t *pBase = nullptr;
	if(Container.m_BufferId != 0)
		glBindBuffer(GL_ARRAY_BUFFER, Container.m_BufferId);
	else
		pBase = Container.m_vConverted.data();

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	if(Container.m_Form == EVertexForm::LAYERED)
	{
		// A tile takes its colour from the draw, so there is no colour array to
		// read and glColor says it once.
		using TVertex = CCommandBuffer::SVertexTex3DStream;
		glDisableClientState(GL_COLOR_ARRAY);
		glVertexPointer(2, GL_FLOAT, sizeof(TVertex), pBase + offsetof(TVertex, m_Pos));
		glTexCoordPointer(3, GL_FLOAT, sizeof(TVertex), pBase + offsetof(TVertex, m_Tex));
	}
	else
	{
		using TVertex = CCommandBuffer::SVertex;
		glVertexPointer(2, GL_FLOAT, sizeof(TVertex), pBase + offsetof(TVertex, m_Pos));
		glTexCoordPointer(2, GL_FLOAT, sizeof(TVertex), pBase + offsetof(TVertex, m_Tex));
		if(pColors != nullptr)
		{
			// The draw tints what the vertices already carry, and fixed
			// function ignores glColor while a colour array is bound. Only the
			// colours are rebuilt for that; the geometry stays where it is.
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glEnableClientState(GL_COLOR_ARRAY);
			glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(CCommandBuffer::SColor), pColors);
			if(Container.m_BufferId != 0)
				glBindBuffer(GL_ARRAY_BUFFER, Container.m_BufferId);
		}
		else
		{
			glEnableClientState(GL_COLOR_ARRAY);
			glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(TVertex), pBase + offsetof(TVertex, m_Color));
		}
	}
}

void CCommandProcessorFragment_OpenGL::UnbindConvertedContainer(const SConvertedBuffer &Container)
{
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	if(Container.m_BufferId != 0)
		glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void CCommandProcessorFragment_OpenGL::SetDrawTransform(const vec2 &Offset, const vec2 &Scale, const vec2 &RotationCenter, float Rotation)
{
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(Offset.x, Offset.y, 0.0f);
	if(Rotation != 0.0f)
	{
		glTranslatef(RotationCenter.x, RotationCenter.y, 0.0f);
		glRotatef(Rotation * 180.0f / pi, 0.0f, 0.0f, 1.0f);
		glTranslatef(-RotationCenter.x, -RotationCenter.y, 0.0f);
	}
	glScalef(Scale.x, Scale.y, 1.0f);
}

void CCommandProcessorFragment_OpenGL::SetTextureTransform(const CCommandBuffer::SState &State, const vec2 &TexScale, bool LayerCoordinates)
{
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	if(LayerCoordinates && !m_Has2DArrayTextures && State.m_Texture.IsValid())
	{
		// A volume is sampled in the middle of the slice the index names, and
		// that is a scale and an offset on the third coordinate.
		const int LayerCount = m_vTextures[State.m_Texture.Id()].m_LayerCount;
		if(LayerCount > 0)
		{
			glTranslatef(0.0f, 0.0f, 0.5f / LayerCount);
			glScalef(1.0f, 1.0f, 1.0f / LayerCount);
		}
	}
	glScalef(TexScale.x, TexScale.y, 1.0f);
	glMatrixMode(GL_MODELVIEW);
}

void CCommandProcessorFragment_OpenGL::ResetTransforms()
{
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

const CCommandBuffer::SColor *CCommandProcessorFragment_OpenGL::FlatColors(const SConvertedBuffer &Container, const ColorRGBA &Color)
{
	const CCommandBuffer::SColor Flat(
		(unsigned char)std::clamp(Color.r * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.g * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.b * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.a * 255.0f, 0.0f, 255.0f));
	m_vTintedColors.assign(Container.m_VertexCount, Flat);
	return m_vTintedColors.data();
}

const CCommandBuffer::SColor *CCommandProcessorFragment_OpenGL::TintColors(const SConvertedBuffer &Container, const ColorRGBA &Tint)
{
	if(Tint.r >= 1.0f && Tint.g >= 1.0f && Tint.b >= 1.0f && Tint.a >= 1.0f)
		return nullptr;
	if(Container.m_vColors.size() != Container.m_VertexCount)
		return nullptr;
	m_vTintedColors.resize(Container.m_VertexCount);
	for(size_t Vertex = 0; Vertex < Container.m_VertexCount; ++Vertex)
	{
		const CCommandBuffer::SColor Color = Container.m_vColors[Vertex];
		m_vTintedColors[Vertex] = CCommandBuffer::SColor(
			(unsigned char)std::clamp(Color.r * Tint.r, 0.0f, 255.0f),
			(unsigned char)std::clamp(Color.g * Tint.g, 0.0f, 255.0f),
			(unsigned char)std::clamp(Color.b * Tint.b, 0.0f, 255.0f),
			(unsigned char)std::clamp(Color.a * Tint.a, 0.0f, 255.0f));
	}
	return m_vTintedColors.data();
}

// ------------ vertex expansion for the fixed function path

ColorRGBA CCommandProcessorFragment_OpenGL::ReadAttribute(const uint8_t *pVertex, const IGraphics::CVertexAttributeDesc &Attribute)
{
	ColorRGBA Result(0.0f, 0.0f, 0.0f, 0.0f);
	const uint8_t *pComponent = pVertex + Attribute.m_Offset;
	for(uint32_t i = 0; i < Attribute.m_ComponentCount && i < 4; ++i)
	{
		float Value = 0.0f;
		switch(Attribute.m_Type)
		{
		case IGraphics::EVertexAttributeType::FLOAT32:
		{
			float Float;
			mem_copy(&Float, pComponent + i * sizeof(float), sizeof(Float));
			Value = Float;
			break;
		}
		case IGraphics::EVertexAttributeType::UINT8:
			Value = pComponent[i];
			if(Attribute.m_Normalized)
				Value /= 255.0f;
			break;
		}
		Result[i] = Value;
	}
	return Result;
}

float CCommandProcessorFragment_OpenGL::LayerCoordinate(const CCommandBuffer::SState &State, float Layer) const
{
	// A layer index addresses an array texture directly, but a volume is
	// sampled in the middle of the slice the index names.
	if(m_Has2DArrayTextures || !State.m_Texture.IsValid())
		return Layer;
	const int LayerCount = m_vTextures[State.m_Texture.Id()].m_LayerCount;
	return LayerCount > 0 ? (Layer + 0.5f) / LayerCount : Layer;
}

uint32_t CCommandProcessorFragment_OpenGL::ReadIndex(const uint8_t *pIndices, size_t IndexStride, uint32_t Position) const
{
	if(IndexStride == sizeof(uint16_t))
	{
		uint16_t Index;
		mem_copy(&Index, pIndices + (size_t)Position * IndexStride, sizeof(Index));
		return Index;
	}
	uint32_t Index;
	mem_copy(&Index, pIndices + (size_t)Position * IndexStride, sizeof(Index));
	return Index;
}

bool CCommandProcessorFragment_OpenGL::CollectIndexedDraw(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer *&pConverted, const uint8_t *&pIndices, size_t &IndexStride, uint32_t &IndexCount)
{
	const SConvertedBuffer *pContainer = ConvertedBufferFor(pCommand->m_VertexBuffer, pCommand->m_Layout);
	if(pContainer == nullptr)
		return false;

	const size_t IndexBufferIndex = pCommand->m_IndexBuffer.Id();
	if(IndexBufferIndex >= m_vEmulatedBuffers.size())
		return false;
	const std::vector<uint8_t> &vIndexData = m_vEmulatedBuffers[IndexBufferIndex].m_vData;
	IndexStride = pCommand->m_IndexType == IGraphics::EIndexType::UINT16 ? sizeof(uint16_t) : sizeof(uint32_t);
	IndexCount = pCommand->m_IndexCount;
	if(pCommand->m_IndexOffset + (size_t)IndexCount * IndexStride > vIndexData.size())
	{
		DropCommand("an indexed draw that reaches past the end of its index buffer");
		return false;
	}
	pIndices = vIndexData.data() + pCommand->m_IndexOffset;
	pConverted = pContainer;
	return true;
}

void CCommandProcessorFragment_OpenGL::DrawExpandedVertices(const CCommandBuffer::SState &State, bool Layered, TWGLuint OverrideTexture)
{
	SetState(State, Layered);
	// The outline half of a split two channel atlas lives in its own image.
	if(OverrideTexture != 0)
		glBindTexture(GL_TEXTURE_2D, OverrideTexture);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	if(Layered)
	{
		const auto *pVertices = m_vExpandedLayeredVertices.data();
		glVertexPointer(2, GL_FLOAT, sizeof(*pVertices), &pVertices->m_Pos);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*pVertices), &pVertices->m_Color);
		glTexCoordPointer(3, GL_FLOAT, sizeof(*pVertices), &pVertices->m_Tex);
		glDrawArrays(GL_TRIANGLES, 0, m_vExpandedLayeredVertices.size());
	}
	else
	{
		const auto *pVertices = m_vExpandedVertices.data();
		glVertexPointer(2, GL_FLOAT, sizeof(*pVertices), &pVertices->m_Pos);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*pVertices), &pVertices->m_Color);
		glTexCoordPointer(2, GL_FLOAT, sizeof(*pVertices), &pVertices->m_Tex);
		glDrawArrays(GL_TRIANGLES, 0, m_vExpandedVertices.size());
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
}

void CCommandProcessorFragment_OpenGL::DrawEmulatedArrayColor(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Container, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount)
{
	const bool HasTransform = pCommand->m_Program == EPipelineProgram::ARRAY_COLOR_TRANSFORM;
	const auto *pColorData = HasTransform ? nullptr : pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColor>();
	const auto *pTransformData = HasTransform ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataArrayColorTransform>() : nullptr;
	if((HasTransform && pTransformData == nullptr) || (!HasTransform && pColorData == nullptr))
	{
		DropCommand("a tile draw without the colour it should be drawn in");
		return;
	}
	const ColorRGBA &Color = HasTransform ? pTransformData->m_Color : pColorData->m_Color;
	const vec2 Offset = HasTransform ? pTransformData->m_Offset : vec2(0.0f, 0.0f);
	const vec2 Scale = HasTransform ? pTransformData->m_Scale : vec2(1.0f, 1.0f);
	const bool Textured = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 2 && IsTexturedState(pCommand->m_State);

	// A tile layer that is not stretched has nothing that changes per vertex:
	// the colour is one colour, and the layer index becomes a volume
	// coordinate through the texture matrix. The converted array is drawn as
	// it lies.
	if(!HasTransform && Container.m_Form == EVertexForm::LAYERED)
	{
		SetState(pCommand->m_State, Textured);
		glColor4f(Color.r, Color.g, Color.b, Color.a);
		SetTextureTransform(pCommand->m_State, vec2(1.0f, 1.0f), true);
		BindConvertedContainer(Container);
		glDrawElements(GL_TRIANGLES, IndexCount, IndexStride == sizeof(uint16_t) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, pIndices);
		UnbindConvertedContainer(Container);
		ResetTransforms();
		return;
	}

	const std::vector<uint8_t> &vVertexData = m_vEmulatedBuffers[Container.m_SourceBuffer.Id()].m_vData;
	const size_t Stride = IGraphics::VertexLayout(Container.m_Layout).m_Stride;
	const size_t VertexCount = vVertexData.size() / Stride;
	const CCommandBuffer::SColor VertexColor(
		(unsigned char)std::clamp(Color.r * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.g * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.b * 255.0f, 0.0f, 255.0f),
		(unsigned char)std::clamp(Color.a * 255.0f, 0.0f, 255.0f));

	m_vExpandedLayeredVertices.clear();
	m_vExpandedLayeredVertices.reserve(IndexCount);
	for(uint32_t i = 0; i < IndexCount; ++i)
	{
		const uint32_t VertexIndex = ReadIndex(pIndices, IndexStride, i);
		if(VertexIndex >= VertexCount)
		{
			DropCommand("a tile draw with an index that names no vertex");
			return;
		}
		const uint8_t *pVertex = vVertexData.data() + (size_t)VertexIndex * Stride;
		const ColorRGBA Position = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[0]);

		CCommandBuffer::SVertexTex3DStream Vertex;
		Vertex.m_Pos = vec2(Position.r * Scale.x + Offset.x, Position.g * Scale.y + Offset.y);
		Vertex.m_Color = VertexColor;
		if(Textured)
		{
			const ColorRGBA TexCoord = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[1]);
			// The border variant stretches a tile over the area it repeats
			// across, and a rotated tile stretches along the other axis.
			const vec2 TexScale = TexCoord.a > 0.0f ? vec2(Scale.y, Scale.x) : Scale;
			Vertex.m_Tex.u = TexCoord.r * TexScale.x;
			Vertex.m_Tex.v = TexCoord.g * TexScale.y;
			Vertex.m_Tex.w = LayerCoordinate(pCommand->m_State, TexCoord.b);
		}
		else
		{
			Vertex.m_Tex = vec3(0.0f, 0.0f, 0.0f);
		}
		m_vExpandedLayeredVertices.push_back(Vertex);
	}
	// A border tile is one tile stretched across the area it repeats over,
	// and its texture coordinates run past 1 accordingly. The program takes
	// fract() of them; fixed function has no such thing, so the volume wraps
	// for this draw and goes back to clamping afterwards - a clamped volume
	// hands the edge texel to the whole area, which is a flat colour where
	// the map's border should be.
	const bool WrapsVolume = HasTransform && Textured && !m_Has2DArrayTextures && pCommand->m_State.m_Texture.IsValid();
	if(WrapsVolume)
	{
		glBindTexture(GL_TEXTURE_3D, m_vTextures[pCommand->m_State.m_Texture.Id()].m_Tex2DArray);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	DrawExpandedVertices(pCommand->m_State, true);
	if(WrapsVolume)
	{
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
}

void CCommandProcessorFragment_OpenGL::DrawEmulatedQuads(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Container, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount)
{
	const bool Grouped = pCommand->m_Program == EPipelineProgram::QUAD_SHARED;
	const uint32_t QuadCount = IndexCount / 6;
	const auto *pQuadData = Grouped ?
					pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataQuadTransform>() :
					pCommand->m_ArrayData.Get<CCommandBuffer::SDrawDataQuadTransform>(QuadCount);
	if(IndexCount % 6 != 0 || pQuadData == nullptr)
	{
		DropCommand("a quad draw without one transform per quad");
		return;
	}
	const bool Textured = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 3 && IsTexturedState(pCommand->m_State);

	// When every quad in the draw shares one offset, that is a matrix and a
	// tint rather than a rebuilt vertex array. A shared rotation is not enough:
	// each quad turns about its own centre, which the vertex carries and a
	// single matrix cannot express.
	if(Grouped && pQuadData->m_Rotation == 0.0f && Container.m_Form == EVertexForm::PRIMITIVE)
	{
		SetState(pCommand->m_State, false);
		SetDrawTransform(pQuadData->m_Offset, vec2(1.0f, 1.0f), vec2(0.0f, 0.0f), 0.0f);
		BindConvertedContainer(Container, TintColors(Container, pQuadData->m_Color));
		glDrawElements(GL_TRIANGLES, IndexCount, IndexStride == sizeof(uint16_t) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, pIndices);
		UnbindConvertedContainer(Container);
		ResetTransforms();
		return;
	}

	const std::vector<uint8_t> &vVertexData = m_vEmulatedBuffers[Container.m_SourceBuffer.Id()].m_vData;
	const size_t Stride = IGraphics::VertexLayout(Container.m_Layout).m_Stride;
	const size_t VertexCount = vVertexData.size() / Stride;
	const size_t BaseQuad = pCommand->m_IndexOffset / (6 * IndexStride);

	m_vExpandedVertices.clear();
	m_vExpandedVertices.reserve(IndexCount);
	for(uint32_t i = 0; i < IndexCount; ++i)
	{
		const uint32_t VertexIndex = ReadIndex(pIndices, IndexStride, i);
		if(VertexIndex >= VertexCount)
		{
			DropCommand("a quad draw with an index that names no vertex");
			return;
		}
		const uint8_t *pVertex = vVertexData.data() + (size_t)VertexIndex * Stride;
		// The shader reads the transform by vertex id, so the quad a vertex
		// belongs to is the only thing that decides which one it gets.
		const size_t Quad = Grouped ? 0 : VertexIndex / 4 - BaseQuad;
		const CCommandBuffer::SDrawDataQuadTransform &Transform = pQuadData[Quad];

		const ColorRGBA Corner = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[0]);
		const ColorRGBA VertexColor = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[1]);

		vec2 Position(Corner.r, Corner.g);
		if(Transform.m_Rotation != 0.0f)
		{
			const vec2 Center(Corner.b, Corner.a);
			const float X = Position.x - Center.x;
			const float Y = Position.y - Center.y;
			Position.x = X * std::cos(Transform.m_Rotation) - Y * std::sin(Transform.m_Rotation) + Center.x;
			Position.y = X * std::sin(Transform.m_Rotation) + Y * std::cos(Transform.m_Rotation) + Center.y;
		}
		Position += Transform.m_Offset;

		CCommandBuffer::SVertex Expanded;
		Expanded.m_Pos = Position;
		Expanded.m_Color = CCommandBuffer::SColor(
			(unsigned char)std::clamp(VertexColor.r * Transform.m_Color.r * 255.0f, 0.0f, 255.0f),
			(unsigned char)std::clamp(VertexColor.g * Transform.m_Color.g * 255.0f, 0.0f, 255.0f),
			(unsigned char)std::clamp(VertexColor.b * Transform.m_Color.b * 255.0f, 0.0f, 255.0f),
			(unsigned char)std::clamp(VertexColor.a * Transform.m_Color.a * 255.0f, 0.0f, 255.0f));
		if(Textured)
		{
			const ColorRGBA TexCoord = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[2]);
			Expanded.m_Tex = vec2(TexCoord.r, TexCoord.g);
		}
		else
		{
			Expanded.m_Tex = vec2(0.0f, 0.0f);
		}
		m_vExpandedVertices.push_back(Expanded);
	}
	DrawExpandedVertices(pCommand->m_State, false);
}

void CCommandProcessorFragment_OpenGL::DrawEmulatedPrimitives(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Container, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount, EPipelineProgram Program)
{
	const auto *pUniformColorData = Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveUniformColor>() : nullptr;
	const auto *pInstancedData = Program == EPipelineProgram::PRIMITIVE_INSTANCED ? pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataPrimitiveInstanced>() : nullptr;
	const auto *pInstances = Program == EPipelineProgram::PRIMITIVE_INSTANCED ? pCommand->m_ArrayData.Get<CCommandBuffer::SInstanceDataPositionScaleRotation>(pCommand->m_InstanceCount) : nullptr;
	if((Program == EPipelineProgram::PRIMITIVE_UNIFORM_COLOR && pUniformColorData == nullptr) ||
		(Program == EPipelineProgram::PRIMITIVE_INSTANCED && (pInstancedData == nullptr || pInstances == nullptr || pCommand->m_InstanceCount == 0)))
	{
		DropCommand("a buffered primitive draw without the data its pipeline needs");
		return;
	}

	const ColorRGBA Color = pUniformColorData != nullptr ? pUniformColorData->m_Color : (pInstancedData != nullptr ? pInstancedData->m_Color : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
	const vec2 RotationCenter = pUniformColorData != nullptr ? pUniformColorData->m_RotationCenter : (pInstancedData != nullptr ? pInstancedData->m_RotationCenter : vec2(0.0f, 0.0f));
	const float Rotation = pUniformColorData != nullptr ? pUniformColorData->m_Rotation : 0.0f;
	const uint32_t InstanceCount = Program == EPipelineProgram::PRIMITIVE_INSTANCED ? pCommand->m_InstanceCount : 1;
	const bool Textured = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 3 && IsTexturedState(pCommand->m_State);

	const std::vector<uint8_t> &vVertexData = m_vEmulatedBuffers[Container.m_SourceBuffer.Id()].m_vData;
	const size_t Stride = IGraphics::VertexLayout(Container.m_Layout).m_Stride;
	const size_t VertexCount = vVertexData.size() / Stride;

	m_vExpandedVertices.clear();
	m_vExpandedVertices.reserve((size_t)IndexCount * InstanceCount);
	for(uint32_t Instance = 0; Instance < InstanceCount; ++Instance)
	{
		for(uint32_t i = 0; i < IndexCount; ++i)
		{
			const uint32_t VertexIndex = ReadIndex(pIndices, IndexStride, i);
			if(VertexIndex >= VertexCount)
			{
				DropCommand("a buffered primitive draw with an index that names no vertex");
				return;
			}
			const uint8_t *pVertex = vVertexData.data() + (size_t)VertexIndex * Stride;
			const ColorRGBA Position = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[0]);
			const ColorRGBA TexCoord = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 2 ? ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[1]) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
			const ColorRGBA VertexColor = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 3 ? ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[2]) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);

			vec2 FinalPos(Position.r, Position.g);
			float InstanceRotation = Rotation;
			if(pInstances != nullptr)
				InstanceRotation = pInstances[Instance].m_Rotation;
			if(InstanceRotation != 0.0f)
			{
				const float X = FinalPos.x - RotationCenter.x;
				const float Y = FinalPos.y - RotationCenter.y;
				FinalPos.x = X * std::cos(InstanceRotation) - Y * std::sin(InstanceRotation) + RotationCenter.x;
				FinalPos.y = X * std::sin(InstanceRotation) + Y * std::cos(InstanceRotation) + RotationCenter.y;
			}
			if(pInstances != nullptr)
			{
				FinalPos *= pInstances[Instance].m_Scale;
				FinalPos += pInstances[Instance].m_Position;
			}

			CCommandBuffer::SVertex Expanded;
			Expanded.m_Pos = FinalPos;
			Expanded.m_Tex = Textured ? vec2(TexCoord.r, TexCoord.g) : vec2(0.0f, 0.0f);
			Expanded.m_Color = CCommandBuffer::SColor(
				(unsigned char)std::clamp(VertexColor.r * Color.r * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(VertexColor.g * Color.g * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(VertexColor.b * Color.b * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(VertexColor.a * Color.a * 255.0f, 0.0f, 255.0f));
			m_vExpandedVertices.push_back(Expanded);
		}
	}
	DrawExpandedVertices(pCommand->m_State, false);
}

void CCommandProcessorFragment_OpenGL::DrawEmulatedDualAtlas(const CCommandBuffer::SCommand_DrawIndexed *pCommand, const SConvertedBuffer &Container, const uint8_t *pIndices, size_t IndexStride, uint32_t IndexCount)
{
	const auto *pDrawData = pCommand->m_DrawData.Get<CCommandBuffer::SDrawDataDualAtlas>();
	if(pDrawData == nullptr || !pCommand->m_State.m_Texture.IsValid())
	{
		DropCommand("a text draw without both of the atlases it composites");
		return;
	}

	const float TextureSize = pDrawData->m_TextureSize != 0.0f ? pDrawData->m_TextureSize : 1.0f;

	// The glyph and its outline are the same geometry twice, in two colours,
	// out of two atlases. Only the colours are rebuilt for that; the texture
	// coordinates are in texels and the texture matrix divides them.
	if(Container.m_Form == EVertexForm::PRIMITIVE)
	{
		for(int Pass = 0; Pass < 2; ++Pass)
		{
			const ColorRGBA &Color = Pass == 0 ? pDrawData->m_SecondaryColor : pDrawData->m_PrimaryColor;
			if(Color.a <= 0.0f)
				continue;
			const CCommandBuffer::SState &State = pCommand->m_State;
			SetState(State, false);
			// The atlas arrived as one two channel image and was split on the
			// way in, because fixed function cannot pick the second channel out
			// here. The outline pass reads that second image.
			if(Pass == 0)
				glBindTexture(GL_TEXTURE_2D, m_vTextures[State.m_Texture.Id()].m_TexSecondChannel);
			SetTextureTransform(State, vec2(1.0f / TextureSize, 1.0f / TextureSize), false);
			// The shader tints only the body by the vertex colour; the outline
			// is the outline colour alone, so the first pass must not multiply.
			BindConvertedContainer(Container, Pass == 0 ? FlatColors(Container, Color) : TintColors(Container, Color));
			glDrawElements(GL_TRIANGLES, IndexCount, IndexStride == sizeof(uint16_t) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, pIndices);
			UnbindConvertedContainer(Container);
			ResetTransforms();
		}
		return;
	}

	const std::vector<uint8_t> &vVertexData = m_vEmulatedBuffers[Container.m_SourceBuffer.Id()].m_vData;
	const size_t Stride = IGraphics::VertexLayout(Container.m_Layout).m_Stride;
	const size_t VertexCount = vVertexData.size() / Stride;

	// One pass composites both atlases; two passes put the outline behind the
	// glyph, which is what the composite comes out as and what the client did
	// before there were shaders to do it in one.
	for(int Pass = 0; Pass < 2; ++Pass)
	{
		const ColorRGBA &Color = Pass == 0 ? pDrawData->m_SecondaryColor : pDrawData->m_PrimaryColor;
		if(Color.a <= 0.0f)
			continue;

		m_vExpandedVertices.clear();
		m_vExpandedVertices.reserve(IndexCount);
		for(uint32_t i = 0; i < IndexCount; ++i)
		{
			const uint32_t VertexIndex = ReadIndex(pIndices, IndexStride, i);
			if(VertexIndex >= VertexCount)
			{
				DropCommand("a text draw with an index that names no vertex");
				return;
			}
			const uint8_t *pVertex = vVertexData.data() + (size_t)VertexIndex * Stride;
			const ColorRGBA Position = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[0]);
			const ColorRGBA TexCoord = ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[1]);
			const ColorRGBA VertexColor = IGraphics::VertexLayout(Container.m_Layout).m_AttributeCount >= 3 ? ReadAttribute(pVertex, IGraphics::VertexLayout(Container.m_Layout).m_aAttributes[2]) : ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);

			CCommandBuffer::SVertex Expanded;
			Expanded.m_Pos = vec2(Position.r, Position.g);
			Expanded.m_Tex = vec2(TexCoord.r / TextureSize, TexCoord.g / TextureSize);
			// Outline pass: the outline colour alone, as the shader does it.
			const ColorRGBA Applied = Pass == 0 ? Color : ColorRGBA(VertexColor.r * Color.r, VertexColor.g * Color.g, VertexColor.b * Color.b, VertexColor.a * Color.a);
			Expanded.m_Color = CCommandBuffer::SColor(
				(unsigned char)std::clamp(Applied.r * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Applied.g * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Applied.b * 255.0f, 0.0f, 255.0f),
				(unsigned char)std::clamp(Applied.a * 255.0f, 0.0f, 255.0f));
			m_vExpandedVertices.push_back(Expanded);
		}

		const CCommandBuffer::SState &State = pCommand->m_State;
		DrawExpandedVertices(State, false, Pass == 0 ? m_vTextures[State.m_Texture.Id()].m_TexSecondChannel : 0);
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_DrawIndexed(const CCommandBuffer::SCommand_DrawIndexed *pCommand)
{
	{
		const SConvertedBuffer *pContainer = nullptr;
		const uint8_t *pIndices = nullptr;
		size_t IndexStride = 0;
		uint32_t IndexCount = 0;
		if(!CollectIndexedDraw(pCommand, pContainer, pIndices, IndexStride, IndexCount) || IndexCount == 0)
			return;
		switch(pCommand->m_Program)
		{
		case EPipelineProgram::ARRAY_COLOR:
		case EPipelineProgram::ARRAY_COLOR_TRANSFORM:
			DrawEmulatedArrayColor(pCommand, *pContainer, pIndices, IndexStride, IndexCount);
			break;
		case EPipelineProgram::QUAD_PER_ITEM:
		case EPipelineProgram::QUAD_SHARED:
			DrawEmulatedQuads(pCommand, *pContainer, pIndices, IndexStride, IndexCount);
			break;
		case EPipelineProgram::DUAL_ATLAS_COMPOSITE:
			DrawEmulatedDualAtlas(pCommand, *pContainer, pIndices, IndexStride, IndexCount);
			break;
		case EPipelineProgram::PRIMITIVE:
		case EPipelineProgram::PRIMITIVE_UNIFORM_COLOR:
		case EPipelineProgram::PRIMITIVE_INSTANCED:
			DrawEmulatedPrimitives(pCommand, *pContainer, pIndices, IndexStride, IndexCount, pCommand->m_Program);
			break;
		default:
			DropCommand("an indexed draw on a pipeline the fixed function path cannot stand in for");
			break;
		}
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_BeginRenderPass(const CCommandBuffer::SCommand_BeginRenderPass *pCommand)
{
	// Legacy backends only advertise the implicit presentation target.
	m_RenderingToTexture = false;
	if(pCommand->m_Desc.m_LoadOp == IGraphics::ERenderPassLoadOp::CLEAR)
	{
		CCommandBuffer::SCommand_Clear Clear;
		Clear.m_Color = pCommand->m_Desc.m_ClearColor;
		Clear.m_ForceClear = true;
		Cmd_Clear(&Clear);
	}
}

void CCommandProcessorFragment_OpenGL::Cmd_EndRenderPass(const CCommandBuffer::SCommand_EndRenderPass *pCommand)
{
	m_RenderingToTexture = false;
}

#endif
