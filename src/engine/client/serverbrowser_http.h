#ifndef ENGINE_CLIENT_SERVERBROWSER_HTTP_H
#define ENGINE_CLIENT_SERVERBROWSER_HTTP_H
#include <base/types.h>

#include <engine/external/json-parser/json.h>

#include <vector>

class CServerInfo;
class IEngine;
class IStorage;
class IHttp;

/**
 * Reads the server list a master answers with.
 *
 * @param pJson The answer.
 * @param pvServers Receives the servers.
 *
 * @return Whether the answer is not a server list.
 */
bool ServerBrowserHttpParse(json_value *pJson, std::vector<CServerInfo> *pvServers);

class IServerBrowserHttp
{
public:
	virtual ~IServerBrowserHttp() = default;

	virtual void Shutdown() = 0;
	virtual void Update() = 0;

	virtual bool IsRefreshing() const = 0;
	virtual bool IsError() const = 0;
	virtual void Refresh() = 0;

	virtual bool GetBestUrl(const char **pBestUrl) const = 0;

	virtual int NumServers() const = 0;
	virtual const CServerInfo &Server(int Index) const = 0;
};

IServerBrowserHttp *CreateServerBrowserHttp(IEngine *pEngine, IStorage *pStorage, IHttp *pHttp, const char *pPreviousBestUrl);
#endif // ENGINE_CLIENT_SERVERBROWSER_HTTP_H
