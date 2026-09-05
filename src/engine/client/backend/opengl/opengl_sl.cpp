#include "opengl_sl.h"

#include <base/detect.h>
#include <base/log.h>

#if defined(BACKEND_AS_OPENGL_ES) || !defined(CONF_BACKEND_OPENGL_ES)

#include <engine/client/backend/embedded_shaders.h>
#include <engine/client/backend/glsl_shader_compiler.h>
#include <engine/graphics.h>

#include <cstring>
#include <string>
#include <vector>

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

bool CGLSL::LoadShader(CGLSLCompiler *pCompiler, const char *pName, int Type)
{
	if(m_IsLoaded)
		return true;

	const SEmbeddedShader *pDialect = FindEmbeddedShader("dialect.glsl");
	const SEmbeddedShader *pShader = FindEmbeddedShader(pName);
	if(pDialect == nullptr || pShader == nullptr)
	{
		log_error("gfx/opengl/shader", "There is no embedded shader named '%s'.", pShader == nullptr ? pName : "dialect.glsl");
		return false;
	}

	std::vector<std::string> vLines;
	const EBackendType BackendType = pCompiler->m_IsOpenGLES ? BACKEND_TYPE_OPENGL_ES : BACKEND_TYPE_OPENGL;
	// The backend that uses programs is written against OpenGL 3.3 core and
	// OpenGL ES 3; the version line comes from the context that was made.
	vLines.push_back(std::string("#version ") + std::to_string(pCompiler->m_OpenGLVersionMajor) + std::to_string(pCompiler->m_OpenGLVersionMinor) + std::to_string(pCompiler->m_OpenGLVersionPatch) + (BackendType == BACKEND_TYPE_OPENGL_ES ? " es\r\n" : " core\r\n"));

	if(BackendType == BACKEND_TYPE_OPENGL_ES)
	{
		if(Type == GL_FRAGMENT_SHADER)
		{
			vLines.emplace_back("precision highp float; \r\n");
			vLines.emplace_back("precision highp sampler2D; \r\n");
			vLines.emplace_back("precision highp sampler3D; \r\n");
			vLines.emplace_back("precision highp samplerCube; \r\n");
			vLines.emplace_back("precision highp samplerCubeShadow; \r\n");
			vLines.emplace_back("precision highp sampler2DShadow; \r\n");
			vLines.emplace_back("precision highp sampler2DArray; \r\n");
			vLines.emplace_back("precision highp sampler2DArrayShadow; \r\n");
		}
	}

	for(const CGLSLCompiler::SGLSLCompilerDefine &Define : pCompiler->m_vDefines)
	{
		vLines.push_back(std::string("#define ") + Define.m_DefineName + std::string(" ") + Define.m_DefineValue + std::string("\r\n"));
	}

	// The source is embedded as text; the compiler still looks at it one
	// line at a time, because that is what the ES rewriting works on. The
	// dialect header goes first, the way the Vulkan build prepends it.
	for(const SEmbeddedShader *pPart : {pDialect, pShader})
	{
		const char *pSource = reinterpret_cast<const char *>(pPart->m_pData);
		const char *pSourceEnd = pSource + pPart->m_Size;
		while(pSource < pSourceEnd)
		{
			const char *pLineEnd = static_cast<const char *>(memchr(pSource, '\n', pSourceEnd - pSource));
			if(pLineEnd == nullptr)
				pLineEnd = pSourceEnd;
			const std::string ReadLine(pSource, pLineEnd);
			std::string Line;
			pCompiler->ParseLine(Line, ReadLine.c_str(), Type == GL_FRAGMENT_SHADER ? GLSL_SHADER_COMPILER_TYPE_FRAGMENT : GLSL_SHADER_COMPILER_TYPE_VERTEX);
			Line.append("\r\n");
			vLines.push_back(Line);
			pSource = pLineEnd + 1;
		}
	}

	std::vector<const char *> vShaderCode(vLines.size());
	for(size_t i = 0; i < vLines.size(); ++i)
	{
		vShaderCode[i] = vLines[i].c_str();
	}

	const TWGLuint ShaderId = glCreateShader(Type);

	glShaderSource(ShaderId, static_cast<GLsizei>(vShaderCode.size()), vShaderCode.data(), nullptr);
	glCompileShader(ShaderId);

	TWGLint CompilationStatus;
	glGetShaderiv(ShaderId, GL_COMPILE_STATUS, &CompilationStatus);

	if(CompilationStatus == GL_FALSE)
	{
		TWGLint LogLength = 0;
		glGetShaderiv(ShaderId, GL_INFO_LOG_LENGTH, &LogLength);
		if(LogLength > 0)
		{
			std::string Log(LogLength, '\0');
			glGetShaderInfoLog(ShaderId, Log.size(), nullptr, &Log.front());
			if(Log.size() >= 2 && Log[Log.size() - 2] == '\n')
			{
				Log[Log.size() - 2] = '\0';
			}
			log_error("gfx/opengl/shader", "Failed to compile shader '%s'. The compiler returned:\n%s", pName, Log.c_str());
		}
		else
		{
			log_error("gfx/opengl/shader", "Failed to compile shader '%s'. The compiler did not return an error.", pName);
		}
		glDeleteShader(ShaderId);
		return false;
	}

	m_IsLoaded = true;
	m_ShaderId = ShaderId;
	return true;
}

bool CGLSL::IsLoaded() const
{
	return m_IsLoaded;
}

TWGLuint CGLSL::GetShaderId() const
{
	return m_ShaderId;
}

CGLSL::~CGLSL()
{
	if(m_IsLoaded)
		glDeleteShader(m_ShaderId);
}

#endif
