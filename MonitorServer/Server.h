#pragma once
#include "HardwareMonitor.h"
#include "IOCP.h"
#include "SettingParser.h"
#include "ProcessMonitor.h"

#include "MonitorProtocol.h"
#include "MonitoringStats.h"
#include "SubnetManager.h"
#include "CLogger.h"

class Server :
    public IOCP
{
	
private:
	
	struct connection
	{
		bool isClient;
		WORD group;
		BYTE serverNo;
		bool isLogin = false;
	};

public:

	Server();
	
	void OnDisconnect(SessionID sessionId) override;
	void OnSessionTimeout(SessionID sessionId,timeoutInfo info) override;
	void BroadcastToClient(CSendBuffer& sendBuffer);
	void OnRecvPacket(SessionID sessionId, CRecvBuffer& buffer) override;
	void AddDataToMonitorInfo(int serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE type, int data);

	void OnMonitorRun() override;
	void OnConnect(SessionID sessionId, const SockAddr_in& info) override;

	
private:

	void MakeMonitorPacket(CSendBuffer& buffer, BYTE serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE type, int value);
	void MakePacketAndBroadcast(int serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE dataType, int data);
	void SendHWInfo();
	void SendChatInfo();
	void SendLoginInfo();
	void SendGameInfo();
	void SendDBQuery(int group, int serverNo, int dataType, int min, int max, double avg, int count);

private:
	static constexpr int MAX_SERVER_NUM = 10;

	SRWLOCK _connectionLock;
	SessionMap<connection> _connections;

	char _clientLoginKey[33] = {0,};

	SubnetManager _subnetManager;

	//MONITOR
	MonitoringStats _monitorInfos[MAX_SERVER_NUM][dfMONITOR_END];
	HardwareMonitor _hwMonitor;
	ProcessMonitor _chatMonitor;
	ProcessMonitor _loginMonitor;
	ProcessMonitor _gameMonitor;

	SettingParser _monitorParser;
	int _monitorUpdateCount = 0 ;
	String _monitorString;

	//DB
	SettingParser _dbParser;
	std::string _dbIP;
	Port _dbPort;
	std::string _dbID;
	std::string _dbPass;
	std::string _basicDatabase;
	bool _useDB = false;

	//LOGGER
	CLogger _logger;
};