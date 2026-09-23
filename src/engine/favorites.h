#ifndef ENGINE_FAVORITES_H
#define ENGINE_FAVORITES_H

#include "kernel.h"

#include <base/types.h>

#include <engine/shared/protocol.h>
#include <engine/shared/server_pin.h>

#include <memory>

class IConfigManager;

class IFavorites : public IInterface
{
	MACRO_INTERFACE("favorites")

protected:
	virtual void OnConfigSave(IConfigManager *pConfigManager) = 0;

public:
	class CEntry
	{
	public:
		int m_NumAddrs;
		NETADDR m_aAddrs[MAX_SERVER_ADDRESSES];
		bool m_AllowPing;
		// What the server was pinned by when it was added, so that a
		// connect from the favorites pins it still when the master's list
		// has nothing for it.
		CServerPin m_Pin;
	};

	virtual TRISTATE IsFavorite(const NETADDR *pAddrs, int NumAddrs) const = 0;
	// Only considers the addresses that are actually favorites.
	virtual TRISTATE IsPingAllowed(const NETADDR *pAddrs, int NumAddrs) const = 0;
	// The pin is kept with the entry, see `CEntry`.
	virtual void Add(const NETADDR *pAddrs, int NumAddrs, const CServerPin *pPin = nullptr) = 0;
	// Only considers the addresses that are actually favorites.
	virtual void AllowPing(const NETADDR *pAddrs, int NumAddrs, bool AllowPing) = 0;
	virtual void Remove(const NETADDR *pAddrs, int NumAddrs) = 0;
	virtual void AllEntries(const CEntry **ppEntries, int *pNumEntries) = 0;

	// Pass the `IFavorites` instance as callback.
	static void ConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData);
};

std::unique_ptr<IFavorites> CreateFavorites();
#endif // ENGINE_FAVORITES_H
