#include "mcp_json.h"

#include <base/str.h>

#include <engine/shared/json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace map_mcp
{
	CJson CJson::Bool(bool Value)
	{
		CJson Json;
		Json.m_Type = BOOL;
		Json.m_Bool = Value;
		return Json;
	}

	CJson CJson::Int(int64_t Value)
	{
		CJson Json;
		Json.m_Type = INT;
		Json.m_Int = Value;
		return Json;
	}

	CJson CJson::Double(double Value)
	{
		CJson Json;
		Json.m_Type = DOUBLE;
		Json.m_Double = Value;
		return Json;
	}

	CJson CJson::Str(const char *pValue)
	{
		CJson Json;
		Json.m_Type = STRING;
		Json.m_String = pValue == nullptr ? "" : pValue;
		return Json;
	}

	CJson CJson::Array()
	{
		CJson Json;
		Json.m_Type = ARRAY;
		return Json;
	}

	CJson CJson::Object()
	{
		CJson Json;
		Json.m_Type = OBJECT;
		return Json;
	}

	CJson CJson::Raw(const std::string &Text)
	{
		CJson Json;
		Json.m_Type = RAW;
		Json.m_String = Text;
		return Json;
	}

	namespace
	{
		CJson Convert(const json_value *pValue)
		{
			switch(pValue->type)
			{
			case json_object:
			{
				CJson Object = CJson::Object();
				for(unsigned i = 0; i < pValue->u.object.length; ++i)
					Object.Set(pValue->u.object.values[i].name, Convert(pValue->u.object.values[i].value));
				return Object;
			}
			case json_array:
			{
				CJson Array = CJson::Array();
				for(unsigned i = 0; i < pValue->u.array.length; ++i)
					Array.Push(Convert(pValue->u.array.values[i]));
				return Array;
			}
			case json_integer:
				return CJson::Int(pValue->u.integer);
			case json_double:
				return CJson::Double(pValue->u.dbl);
			case json_string:
				return CJson::Str(pValue->u.string.ptr);
			case json_boolean:
				return CJson::Bool(pValue->u.boolean != 0);
			case json_null:
			case json_none:
			default:
				return CJson::Null();
			}
		}
	} // namespace

	bool CJson::Parse(const char *pText, CJson *pOut, std::string *pError)
	{
		json_settings Settings = {};
		char aError[256] = "";
		const std::unique_ptr<json_value, decltype(&json_value_free)> pParsed(
			JsonParseEx(&Settings, pText, str_length(pText), aError), json_value_free);
		if(pParsed == nullptr)
		{
			*pError = aError[0] == '\0' ? "not JSON" : aError;
			return false;
		}
		*pOut = Convert(pParsed.get());
		return true;
	}

	int64_t CJson::AsInt(int64_t Default) const
	{
		if(m_Type == INT)
			return m_Int;
		if(m_Type == DOUBLE && std::floor(m_Double) == m_Double && std::fabs(m_Double) < 9.0e15)
			return (int64_t)m_Double;
		return Default;
	}

	double CJson::AsDouble(double Default) const
	{
		if(m_Type == INT)
			return (double)m_Int;
		if(m_Type == DOUBLE)
			return m_Double;
		return Default;
	}

	const CJson &CJson::At(size_t Index) const
	{
		static const CJson s_Null;
		return m_Type == ARRAY && Index < m_vItems.size() ? m_vItems[Index] : s_Null;
	}

	const CJson &CJson::Get(const char *pKey) const
	{
		static const CJson s_Null;
		if(m_Type != OBJECT)
			return s_Null;
		for(const auto &[Key, Value] : m_vMembers)
		{
			if(Key == pKey)
				return Value;
		}
		return s_Null;
	}

	bool CJson::Has(const char *pKey) const
	{
		if(m_Type != OBJECT)
			return false;
		return std::any_of(m_vMembers.begin(), m_vMembers.end(), [pKey](const auto &Member) { return Member.first == pKey; });
	}

	const std::string &CJson::KeyAt(size_t Index) const
	{
		static const std::string s_Empty;
		return m_Type == OBJECT && Index < m_vMembers.size() ? m_vMembers[Index].first : s_Empty;
	}

	CJson &CJson::Set(const char *pKey, CJson Value)
	{
		if(m_Type != OBJECT)
		{
			*this = Object();
		}
		for(auto &[Key, Member] : m_vMembers)
		{
			if(Key == pKey)
			{
				Member = std::move(Value);
				return *this;
			}
		}
		m_vMembers.emplace_back(pKey, std::move(Value));
		return *this;
	}

	CJson &CJson::Push(CJson Value)
	{
		if(m_Type != ARRAY)
		{
			*this = Array();
		}
		m_vItems.push_back(std::move(Value));
		return *this;
	}

	void CJson::WriteString(std::string &Out, const char *pValue)
	{
		Out += '"';
		for(const char *p = pValue; *p != '\0'; ++p)
		{
			const unsigned char c = (unsigned char)*p;
			switch(c)
			{
			case '"': Out += "\\\""; break;
			case '\\': Out += "\\\\"; break;
			case '\n': Out += "\\n"; break;
			case '\r': Out += "\\r"; break;
			case '\t': Out += "\\t"; break;
			case '\b': Out += "\\b"; break;
			case '\f': Out += "\\f"; break;
			default:
				if(c < 0x20)
				{
					char aBuf[8];
					str_format(aBuf, sizeof(aBuf), "\\u%04x", c);
					Out += aBuf;
				}
				else
				{
					Out += (char)c;
				}
			}
		}
		Out += '"';
	}

	void CJson::Write(std::string &Out) const
	{
		switch(m_Type)
		{
		case NUL: Out += "null"; break;
		case BOOL: Out += m_Bool ? "true" : "false"; break;
		case INT: Out += std::to_string(m_Int); break;
		case DOUBLE:
		{
			if(std::isfinite(m_Double))
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "%.17g", m_Double);
				// A number that came out whole is still a number, but one that
				// could not be read back as a double is not.
				if(str_find(aBuf, ".") == nullptr && str_find(aBuf, "e") == nullptr)
					str_append(aBuf, ".0");
				Out += aBuf;
			}
			else
			{
				Out += "null";
			}
			break;
		}
		case STRING: WriteString(Out, m_String.c_str()); break;
		case RAW: Out += m_String; break;
		case ARRAY:
		{
			Out += '[';
			bool First = true;
			for(const CJson &Item : m_vItems)
			{
				if(!First)
					Out += ',';
				First = false;
				Item.Write(Out);
			}
			Out += ']';
			break;
		}
		case OBJECT:
		{
			Out += '{';
			bool First = true;
			for(const auto &[Key, Value] : m_vMembers)
			{
				if(!First)
					Out += ',';
				First = false;
				WriteString(Out, Key.c_str());
				Out += ':';
				Value.Write(Out);
			}
			Out += '}';
			break;
		}
		}
	}

	std::string CJson::Serialize() const
	{
		std::string Out;
		Write(Out);
		return Out;
	}
} // namespace map_mcp
