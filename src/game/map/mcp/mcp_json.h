#ifndef GAME_MAP_MCP_MCP_JSON_H
#define GAME_MAP_MCP_MCP_JSON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace map_mcp
{
	/**
	 * A JSON value that can be built up, looked into and written out.
	 *
	 * The engine's writer streams JSON and its parser hands back a tree that
	 * cannot be changed, and a tool result is neither: it is put together
	 * from pieces, some of which - a structure the document already wrote as
	 * text - are JSON already. So this is a small tree of its own, with a
	 * `RAW` node for text that is JSON and goes out as it is.
	 *
	 * Objects keep the order their keys were set in, so that what a model
	 * reads is in the order it was meant to be read in.
	 */
	class CJson
	{
	public:
		enum EType
		{
			NUL,
			BOOL,
			INT,
			DOUBLE,
			STRING,
			ARRAY,
			OBJECT,
			RAW,
		};

		CJson() = default;

		static CJson Null() { return CJson(); }
		static CJson Bool(bool Value);
		static CJson Int(int64_t Value);
		static CJson Double(double Value);
		static CJson Str(const char *pValue);
		static CJson Str(const std::string &Value) { return Str(Value.c_str()); }
		static CJson Array();
		static CJson Object();
		/** Text that is JSON already and is written out as it is. */
		static CJson Raw(const std::string &Text);

		/**
		 * Reads JSON text into a tree.
		 *
		 * @param pText The text.
		 * @param pOut Where the tree goes.
		 * @param pError What was wrong with the text, if anything.
		 *
		 * @return Whether the text was JSON.
		 */
		static bool Parse(const char *pText, CJson *pOut, std::string *pError);

		EType Type() const { return m_Type; }
		bool IsNull() const { return m_Type == NUL; }
		bool IsBool() const { return m_Type == BOOL; }
		bool IsInt() const { return m_Type == INT; }
		bool IsNumber() const { return m_Type == INT || m_Type == DOUBLE; }
		bool IsString() const { return m_Type == STRING; }
		bool IsArray() const { return m_Type == ARRAY; }
		bool IsObject() const { return m_Type == OBJECT; }

		bool AsBool(bool Default = false) const { return m_Type == BOOL ? m_Bool : Default; }
		int64_t AsInt(int64_t Default = 0) const;
		double AsDouble(double Default = 0.0) const;
		const std::string &AsString() const { return m_String; }
		const char *AsCStr(const char *pDefault = "") const { return m_Type == STRING ? m_String.c_str() : pDefault; }

		/** For an array or an object, how many members it has. */
		size_t Size() const { return m_Type == ARRAY ? m_vItems.size() : m_Type == OBJECT ? m_vMembers.size() :
												    0; }
		/** A member of an array, or null past its end. */
		const CJson &At(size_t Index) const;
		/** A member of an object, or null where there is none. */
		const CJson &Get(const char *pKey) const;
		bool Has(const char *pKey) const;
		/** The key of the `Index`th member of an object. */
		const std::string &KeyAt(size_t Index) const;

		/** Sets a member of an object, in place of one of that name. */
		CJson &Set(const char *pKey, CJson Value);
		/** Adds a member to an array. */
		CJson &Push(CJson Value);

		/** The value as text, on one line. */
		std::string Serialize() const;

		/** Writes a string as a JSON string, quotes and escapes included. */
		static void WriteString(std::string &Out, const char *pValue);

	private:
		void Write(std::string &Out) const;

		EType m_Type = NUL;
		bool m_Bool = false;
		int64_t m_Int = 0;
		double m_Double = 0.0;
		std::string m_String;
		std::vector<CJson> m_vItems;
		std::vector<std::pair<std::string, CJson>> m_vMembers;
	};
} // namespace map_mcp

#endif // GAME_MAP_MCP_MCP_JSON_H
