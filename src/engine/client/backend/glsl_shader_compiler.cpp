#include "glsl_shader_compiler.h"

#include <base/dbg.h>
#include <base/str.h>

#include <engine/graphics.h>

CGLSLCompiler::CGLSLCompiler(int OpenGLVersionMajor, int OpenGLVersionMinor, int OpenGLVersionPatch, bool IsOpenGLES, float TextureLODBias)
{
	m_OpenGLVersionMajor = OpenGLVersionMajor;
	m_OpenGLVersionMinor = OpenGLVersionMinor;
	m_OpenGLVersionPatch = OpenGLVersionPatch;

	m_IsOpenGLES = IsOpenGLES;

	m_TextureLODBias = TextureLODBias;
}

void CGLSLCompiler::AddDefine(const std::string &DefineName, const std::string &DefineValue)
{
	m_vDefines.emplace_back(DefineName, DefineValue);
}

void CGLSLCompiler::AddDefine(const char *pDefineName, const char *pDefineValue)
{
	AddDefine(std::string(pDefineName), std::string(pDefineValue));
}

void CGLSLCompiler::ClearDefines()
{
	m_vDefines.clear();
}

void CGLSLCompiler::ParseLine(std::string &Line, const char *pReadLine, EGLSLShaderCompilerType Type)
{
	if(m_IsOpenGLES)
	{
		const char *pBuff = pReadLine;
		char aTmpStr[1024];
		size_t TmpStrSize = 0;
		while(*pBuff)
		{
			while(*pBuff && str_isspace(*pBuff))
			{
				Line.append(1, *pBuff);
				++pBuff;
			}

			while(*pBuff && TmpStrSize < sizeof(aTmpStr) - 1 && !str_isspace(*pBuff) && *pBuff != '(' && *pBuff != '.')
			{
				aTmpStr[TmpStrSize++] = *pBuff;
				++pBuff;
			}

			if(TmpStrSize > 0)
			{
				aTmpStr[TmpStrSize] = '\0';
				TmpStrSize = 0;

				if(str_comp(aTmpStr, "noperspective") == 0)
				{
					// GLES does not support noperspective. Drop it to use the default (smooth) inexplicitly because shaders fail to compile on iOS otherwise.
					// The centroid qualifier has to be dropped with it, because the qualifiers of the
					// vertex shader outputs and the fragment shader inputs must match exactly on GLES.
					if(const char *pCentroidEnd = str_startswith(str_skip_whitespaces_const(pBuff), "centroid"))
					{
						pBuff = pCentroidEnd;
					}
					Line.append(pBuff);
					return;
				}
				// since GLES doesn't support texture LOD bias as global state, use the shader function instead(since GLES 3.0 uses shaders only anyway)
				else if(str_comp(aTmpStr, "texture") == 0)
				{
					Line.append("texture");
					// check opening and closing brackets to find the end
					int CurBrackets = 1;
					while(*pBuff && *pBuff != '(')
					{
						Line.append(1, *pBuff);
						++pBuff;
					}

					if(*pBuff)
					{
						Line.append(1, *pBuff);
						++pBuff;
					}

					while(*pBuff)
					{
						if(*pBuff == '(')
							++CurBrackets;
						if(*pBuff == ')')
							--CurBrackets;

						if(CurBrackets == 0)
						{
							// found end
							Line.append(std::string(", ") + std::to_string(m_TextureLODBias) + ")");
							++pBuff;
							break;
						}
						else
						{
							Line.append(1, *pBuff);
						}
						++pBuff;
					}

					Line.append(pBuff);
					return;
				}
				else
				{
					Line.append(aTmpStr);
				}
			}

			if(*pBuff)
			{
				Line.append(1, *pBuff);
				++pBuff;
			}
		}
	}
	else
	{
		Line = pReadLine;
	}
}
