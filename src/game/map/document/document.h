#ifndef GAME_MAP_DOCUMENT_DOCUMENT_H
#define GAME_MAP_DOCUMENT_DOCUMENT_H

#include <base/dbg.h>

#include <game/map/document/history.h>
#include <game/map/document/map_state.h>

#include <optional>
#include <string>
#include <utility>

namespace map_document
{
	/**
	 * One map being edited: the versions it has been in, and the one being
	 * made right now.
	 *
	 * Everything that changes the map goes through a transaction, and a
	 * transaction is the only way to change it - so there is no way to edit a
	 * map that does not end in a version, which is what makes undo complete
	 * by build rather than by discipline.
	 *
	 * While a transaction is open, what the map *is* is the half-made version
	 * inside it. That is the preview a tool needs: a slider that is being
	 * dragged changes what is drawn twenty times, and writes one entry when
	 * it is let go. The preview cannot outlive the transaction, because it is
	 * the transaction.
	 *
	 * Transactions nest, because a tool may call another one that has a
	 * transaction of its own - an automapper run inside a brush stroke. The
	 * outermost one decides: it writes the entry, and aborting anywhere
	 * inside throws the whole thing away rather than half of it.
	 */
	class CDocument
	{
	public:
		explicit CDocument(CMapState Opened, const char *pLabel = "Opened") :
			m_History(std::move(Opened), pLabel)
		{
		}

		/**
		 * The map as it stands: the version being made if a transaction is
		 * open, and the version the history is at otherwise. This is what is
		 * drawn, and what would be saved.
		 */
		const CMapState &Map() const { return m_Edit.has_value() ? *m_Edit : m_History.Current(); }

		const CHistory &History() const { return m_History; }

		/** Whether a change is being made right now. */
		bool IsEditing() const { return m_Edit.has_value(); }

		/**
		 * Starts a change, or joins the one that is already being made.
		 *
		 * The label is the one the history will carry, and only the outermost
		 * transaction gets to name it - the automapper that runs inside a
		 * stroke does not rename the stroke.
		 */
		void Begin(const char *pLabel)
		{
			if(m_Depth == 0)
			{
				m_Edit = m_History.Current();
				m_Label = pLabel;
			}
			++m_Depth;
		}

		/**
		 * The version being made, to be changed - see `CMapState` for how.
		 * Only while a transaction is open.
		 */
		CMapState &Edit()
		{
			dbg_assert(m_Edit.has_value(), "Nothing may change the map outside a transaction");
			return *m_Edit;
		}

		/**
		 * Closes the change. The outermost one writes it down - unless it
		 * changed nothing at all, because a slider that was dragged and put
		 * back where it was is not something to undo.
		 */
		void Commit()
		{
			dbg_assert(m_Depth > 0, "Nothing is being changed");
			--m_Depth;
			if(m_Depth > 0)
				return;
			if(!Same(*m_Edit, m_History.Current()))
				m_History.Push(std::move(*m_Edit), m_Label.c_str());
			m_Edit.reset();
		}

		/**
		 * Throws the change away, however deep in it we are: a tool that
		 * gives up gives up the whole change, not its own part of it.
		 */
		void Abort()
		{
			dbg_assert(m_Depth > 0, "Nothing is being changed");
			m_Depth = 0;
			m_Edit.reset();
		}

		bool CanUndo() const { return m_History.CanUndo(); }
		bool CanRedo() const { return m_History.CanRedo(); }

		/**
		 * A step back or forward through the versions. Not while something is
		 * being changed - what that would mean is a question no tool has ever
		 * needed answered.
		 */
		bool Undo()
		{
			dbg_assert(!IsEditing(), "The map cannot be stepped back while it is being changed");
			return m_History.Undo();
		}

		bool Redo()
		{
			dbg_assert(!IsEditing(), "The map cannot be stepped forward while it is being changed");
			return m_History.Redo();
		}

		void JumpTo(size_t Index)
		{
			dbg_assert(!IsEditing(), "The map cannot be stepped about while it is being changed");
			m_History.JumpTo(Index);
		}

		void SetHistoryLimits(uint64_t MaxBytes, size_t MaxEntries) { m_History.SetLimits(MaxBytes, MaxEntries); }

	private:
		/**
		 * Whether two versions hold the same map.
		 *
		 * Nodes that are the same node are not looked into, which is nearly
		 * all of them, and a layer that was replaced is compared against the
		 * one it replaced - because the way a map is changed makes a new node
		 * whether or not anything in it came out different. A tool that put a
		 * value back where it was, or painted the tile that was already
		 * there, is this case.
		 */
		static bool Same(const CMapState &One, const CMapState &Other)
		{
			if(One.m_vpGroups.size() != Other.m_vpGroups.size() ||
				One.m_vpEnvelopes.size() != Other.m_vpEnvelopes.size() ||
				One.m_vpImages.size() != Other.m_vpImages.size() ||
				One.m_vpSounds.size() != Other.m_vpSounds.size())
				return false;
			for(size_t i = 0; i < One.m_vpGroups.size(); ++i)
			{
				// The same node is the same group without looking; a node
				// that was replaced has to be looked into, because replacing
				// a layer makes new nodes whether or not anything in it is
				// different - that is the whole of what this catches.
				if(One.m_vpGroups[i] == Other.m_vpGroups[i])
					continue;
				if(*One.m_vpGroups[i] != *Other.m_vpGroups[i])
					return false;
			}
			for(size_t i = 0; i < One.m_vpEnvelopes.size(); ++i)
			{
				if(One.m_vpEnvelopes[i] != Other.m_vpEnvelopes[i])
					return false;
			}
			for(size_t i = 0; i < One.m_vpImages.size(); ++i)
			{
				if(One.m_vpImages[i] != Other.m_vpImages[i])
					return false;
			}
			for(size_t i = 0; i < One.m_vpSounds.size(); ++i)
			{
				if(One.m_vpSounds[i] != Other.m_vpSounds[i])
					return false;
			}
			return Same(One.m_Info, Other.m_Info);
		}

		static bool Same(const CMapInfo &One, const CMapInfo &Other)
		{
			return One.m_Author == Other.m_Author &&
			       One.m_MapVersion == Other.m_MapVersion &&
			       One.m_Credits == Other.m_Credits &&
			       One.m_License == Other.m_License &&
			       One.m_Settings.Id() == Other.m_Settings.Id();
		}

		CHistory m_History;
		// Held only while a transaction is open, and it is what the map is
		// for as long as it is.
		std::optional<CMapState> m_Edit;
		std::string m_Label;
		int m_Depth = 0;
	};
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_DOCUMENT_H
