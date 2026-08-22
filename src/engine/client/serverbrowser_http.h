#ifndef ENGINE_CLIENT_SERVERBROWSER_HTTP_H
#define ENGINE_CLIENT_SERVERBROWSER_HTTP_H
#include <base/types.h>

#include <engine/external/json-parser/json.h>

#include <vector>

class CServerInfo;
class IEngine;
class IStorage;
class IHttp;

bool ServerBrowserHttpParse(json_value *pJson, std::vector<CServerInfo> *pvServers);

class IServerBrowserHttp
{
public:
	virtual ~IServerBrowserHttp() = default;

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
