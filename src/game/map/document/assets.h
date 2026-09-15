#ifndef GAME_MAP_DOCUMENT_ASSETS_H
#define GAME_MAP_DOCUMENT_ASSETS_H

#include <game/map/document/shared_list.h>
#include <game/mapitems.h>

#include <cstdint>
#include <string>
#include <unordered_set>

namespace map_document
{
	/**
	 * One envelope: what a value does over time, for whoever points at it.
	 *
	 * The points are a list, so a version that drags one point copies the points
	 * of that one envelope - see `CSharedList`. Bezier tangents are kept with
	 * every point whether the envelope uses them or not, the way the editor
	 * holds them today, because which of the two ways they are written to a file
	 * is decided when the file is written, not while it is edited.
	 */
	class CEnvelope
	{
	public:
		std::string m_Name;
		/** 1 for sound, 3 for position, 4 for colour. */
		int m_Channels = 4;
		/** Whether it runs off the map's clock rather than the player's. */
		bool m_Synchronized = false;

		CSharedList<CEnvPoint_runtime> m_Points;

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const { return m_Points.BytesOnce(Seen); }
	};

	/**
	 * One image of the map.
	 *
	 * An embedded image carries its pixels, and they are the biggest thing in
	 * most map files, so they are shared the same way everything else is: a
	 * version that renames an image does not copy its pixels. An external image
	 * carries only the name it is looked up by.
	 */
	class CImage
	{
	public:
		std::string m_Name;
		/** Whether the pixels live in the file or are looked up by name. */
		bool m_External = true;

		int m_Width = 0;
		int m_Height = 0;
		/** RGBA, `m_Width * m_Height * 4` bytes, empty for an external image. */
		CSharedList<uint8_t> m_Data;

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const { return m_Data.BytesOnce(Seen); }
	};

	/**
	 * One sound of the map, embedded as the opus file it was read from.
	 */
	class CSound
	{
	public:
		std::string m_Name;
		bool m_External = false;

		CSharedList<uint8_t> m_Data;

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const { return m_Data.BytesOnce(Seen); }
	};

	/**
	 * What the map says about itself, and what it asks of a server running it.
	 *
	 * Small enough to copy with every version that touches it, which is what the
	 * editor does today without an undo entry - here it is a version like any
	 * other, because there is no way to change the map that is not one.
	 */
	class CMapInfo
	{
	public:
		std::string m_Author;
		std::string m_MapVersion;
		std::string m_Credits;
		std::string m_License;

		/** Console lines a server runs when it loads the map. */
		CSharedList<std::string> m_Settings;

		uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
		{
			// Nothing when the list has been counted already or holds nothing,
			// and the text of the settings only when the list itself is new -
			// otherwise every version that shares them would count them again.
			const uint64_t List = m_Settings.BytesOnce(Seen);
			if(List == 0)
				return 0;
			uint64_t Total = List;
			for(const std::string &Setting : m_Settings.All())
			{
				Total += Setting.capacity();
			}
			return Total;
		}
	};
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_ASSETS_H
