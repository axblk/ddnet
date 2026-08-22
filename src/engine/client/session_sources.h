#ifndef ENGINE_CLIENT_SESSION_SOURCES_H
#define ENGINE_CLIENT_SESSION_SOURCES_H

#include "connection.h"
#include "session.h"
#include "stream.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/client/enums.h>
#include <engine/serverbrowser.h>
#include <engine/shared/translation_context.h>

#include <functional>
#include <string>
#include <vector>

class CServerCapabilities
{
public:
	bool m_ChatTimeoutCode = false;
	bool m_AnyPlayerFlag = false;
	bool m_PingEx = false;
	bool m_AllowDummy = false;
	bool m_SyncWeaponInput = false;
};

class CSessionSourceBase : public IGameSessionSource
{
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;
	CServerInfo m_ServerInfo = {};
	bool m_Sixup = false;
	CTranslationContext m_TranslationContext;
	std::function<void()> m_UpdateFunc;
	std::function<void(const char *)> m_StopFunc;
	std::string m_StopReason;
	bool m_Updating = false;

	void Stop();

public:
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	bool SetState(ESessionState State) override;
	void Fail(const char *pError) override;
	void Update() override;
	void RequestStop(const char *pReason = nullptr) override;
	void SetLifecycleCallbacks(std::function<void()> UpdateFunc, std::function<void(const char *)> StopFunc);
	/**
	 * The connection state of one of this source's streams, so that everything
	 * which only reads ticks and snapshots works the same for a demo and for a
	 * server without knowing which of the two it has.
	 *
	 * @return `nullptr` when the stream does not belong to this source.
	 */
	virtual CConnection *StreamConnection(CStreamId Id) = 0;
	virtual const CConnection *StreamConnection(CStreamId Id) const = 0;
	/**
	 * The stream at a position, counted the way the legacy connection numbers
	 * count, and the position of a stream.
	 */
	virtual CStreamId StreamIdForIndex(int Index) const = 0;
	virtual int IndexForStream(CStreamId Id) const = 0;
	/**
	 * The snapshot delta this source unpacks with, so that a static size
	 * registered by the game reaches every session the same way.
	 */
	virtual CSnapshotDelta &SnapshotDelta(bool Sixup) = 0;
	/**
	 * Whether the other side applies the input of a tick to the weapon of that
	 * same tick. A demo has no other side, and the smaller prediction margin
	 * a server without it gets is the safe answer.
	 */
	virtual bool SyncWeaponInput() const { return false; }
	bool IsUpdating() const { return m_Updating; }
	CServerInfo &ServerInfo() { return m_ServerInfo; }
	const CServerInfo &ServerInfo() const { return m_ServerInfo; }
	bool IsSixup() const { return m_Sixup; }
	void SetSixup(bool Sixup) { m_Sixup = Sixup; }
	CTranslationContext &TranslationContext() { return m_TranslationContext; }
	const CTranslationContext &TranslationContext() const { return m_TranslationContext; }
	void ResetMetadata()
	{
		m_ServerInfo = {};
		m_Sixup = false;
		m_TranslationContext.Reset();
	}
};

#endif // ENGINE_CLIENT_SESSION_SOURCES_H
