#include "stdafx.h"
#include "Server.h"

#include <ctime>
#include <format>

#include "CRecvBuffer.h"
#include "CSendBuffer.h"
#include "LockGuard.h"
#include "DBConnection.h"

Server::Server() : _chatMonitor(L"IOCP_ChatServer"),_loginMonitor(L"IOCP_LoginServer"),_gameMonitor(L"Echo_group"), _logger(L"ContentLog.txt")
{
	_monitorString.reserve(1000);
	InitializeSRWLock(&_connectionLock);

	//Monitor
	_monitorParser.Init(L"monitorSetting.txt");
	std::string key;
	_monitorParser.GetValue(L"monitor.key", key);
	if (key.length() != 32)
	{
		throw exception("Key length require 32");
	}
	key.copy(_clientLoginKey, 32, 0);


	//DB
	_dbParser.Init(L"dbSetting.txt");
	_dbParser.GetValue(L"DB.ID", _dbID);
	_dbParser.GetValue(L"DB.IP", _dbIP);
	_dbParser.GetValue(L"DB.Password", _dbPass);
	_dbParser.GetValue(L"DB.Port", _dbPort);
	_dbParser.GetValue(L"DB.Database", _basicDatabase);
	_dbParser.GetValue(L"DB.UseDB", _useDB);
}



void Server::OnDisconnect(SessionID sessionId)
{
	printf("disconnect\n");

	ExclusiveLockGuard lock(_connectionLock);

	_connections.erase(sessionId);
}

void Server::OnSessionTimeout(SessionID sessionId, timeoutInfo info)
{
	
	DisconnectSession(sessionId);
}



void Server::OnRecvPacket(SessionID sessionId, CRecvBuffer& buffer)
{
	AcquireSRWLockShared(&_connectionLock);
	auto result = _connections.find(sessionId);
	ReleaseSRWLockShared(&_connectionLock);

	while ( buffer.CanPopSize() > 0 )
	{
		WORD type;
		buffer >> type;
		if ( result->second.isClient )
		{
			switch ( type )
			{
				case en_PACKET_CS_MONITOR_TOOL_REQ_LOGIN:
				{
					char sessionKey[33] = { 0, };

					buffer.GetCstr(sessionKey, 32);
					auto loginResult = en_PACKET_CS_MONITOR_TOOL_RES_LOGIN::dfMONITOR_TOOL_LOGIN_OK;
					if ( strcmp(sessionKey, _clientLoginKey) != 0 )
					{
						loginResult = dfMONITOR_TOOL_LOGIN_ERR_SESSIONKEY;

					}

					CSendBuffer& sendBuffer = *CSendBuffer::Alloc();
					sendBuffer << en_PACKET_CS_MONITOR_TOOL_RES_LOGIN << loginResult;
					SendPacket(sessionId, &sendBuffer);

					sendBuffer.Release();

					if ( loginResult == dfMONITOR_TOOL_LOGIN_ERR_SESSIONKEY )
					{
						DisconnectSession(sessionId);
						break;
					}
					{
						SharedLockGuard lock(_connectionLock);
						const auto result = _connections.find(sessionId);
						result->second.isLogin = true;
					}
						
					SetTimeout(sessionId, 0);
					printf("ClientLogin %lld\n", sessionId.id);
				}
				break;
				default:
					DisconnectSession(sessionId);
					break;
			}
			continue;
		}

		//서버인 경우
		switch ( type )
		{
			case en_PACKET_SS_MONITOR_LOGIN:
			{
				WORD group;
				BYTE serverNo;
				buffer >> group >> serverNo;
				result->second.group = group;
				result->second.serverNo = serverNo;
				printf("ServerLogin serverNo : %d , sessionID : %lld\n", serverNo, sessionId.id );
			}
			break;
			case en_PACKET_SS_MONITOR_DATA_UPDATE:
			{
				WORD group;
				BYTE serverNo;
				BYTE dataType;
				int value;
				int recvTime;

				buffer >> group >> serverNo >> dataType >> value >> recvTime;

				AddDataToMonitorInfo(serverNo,(en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE)dataType,value);
				


				MakePacketAndBroadcast(serverNo, (en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE)dataType, value);
			}
			break;
		}
		
	}

}

void Server::AddDataToMonitorInfo(int serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE type, int data) {
	_monitorInfos[serverType][type].AddData(data);
}

String getTimeString()
{
	std::time_t now_c = chrono::system_clock::to_time_t(chrono::system_clock::now());
	tm local_time;
	localtime_s(&local_time, &now_c);
	return std::format(L"{}-{}-{} {}:{}:{:02d}\n"
					   , local_time.tm_year + 1900, local_time.tm_mon+1, local_time.tm_mday
					   , local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
}

void Server::OnMonitorRun()
{
	_hwMonitor.Update();
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_CPU_TOTAL, (int) _hwMonitor.TotalProcessorTime());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NONPAGED_MEMORY, ( int ) _hwMonitor.NonPagedMb());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NETWORK_RECV, ( int ) _hwMonitor.RecvKBytes());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NETWORK_SEND, ( int ) _hwMonitor.SendKBytes());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_AVAILABLE_MEMORY, ( int ) _hwMonitor.AvailableMb());
	SendHWInfo();
	
	_chatMonitor.Update();
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_CHAT, dfMONITOR_DATA_TYPE_CHAT_SERVER_CPU, ( int ) _chatMonitor.TotalProcessorTime());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_CHAT, dfMONITOR_DATA_TYPE_CHAT_SERVER_MEM,static_cast<int>(_chatMonitor.UseMemoryMB()));
	SendChatInfo();

	_loginMonitor.Update();
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_LOGIN, dfMONITOR_DATA_TYPE_LOGIN_SERVER_CPU, ( int ) _loginMonitor.TotalProcessorTime());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_LOGIN, dfMONITOR_DATA_TYPE_LOGIN_SERVER_MEM, static_cast< int >( _loginMonitor.UseMemoryMB() ));
	SendLoginInfo();

	_gameMonitor.Update();
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_GAME, dfMONITOR_DATA_TYPE_GAME_SERVER_CPU, ( int ) _gameMonitor.TotalProcessorTime());
	AddDataToMonitorInfo(en_Server_TYPE::en_SERVER_GAME, dfMONITOR_DATA_TYPE_GAME_SERVER_MEM, static_cast< int >( _gameMonitor.UseMemoryMB() ));
	SendGameInfo();


	if ( ++_monitorUpdateCount == 60 )
	{
		_monitorUpdateCount = 0;
		
		_monitorString += L"===================================\n";
		_monitorString += getTimeString();
		_monitorString += L"===================================\n";

		for ( int serverNo = 0; serverNo < MAX_SERVER_NUM; serverNo++ )
		{
			for ( int i = 0; i < dfMONITOR_END; i++ )
			{
				if ( _monitorInfos[serverNo][i].Count() == 0 )
					continue;
				
				if ( _useDB ) 
				{
					SendDBQuery(1, serverNo, i, _monitorInfos[serverNo][i].Min(), _monitorInfos[serverNo][i].Max(), _monitorInfos[serverNo][i].Avg(), _monitorInfos[serverNo][i].Count());
				}

				_monitorString += format(L"{:2d}| min : {:6d}, max : {:6d}, avg : {:6.2f}, count : {:6d}\n"
										 , i, _monitorInfos[serverNo][i].Min(), _monitorInfos[serverNo][i].Max(), _monitorInfos[serverNo][i].Avg(), _monitorInfos[serverNo][i].Count());

				_monitorInfos[serverNo][i].Clear();
			}
		}

		wprintf(_monitorString.c_str());
		_monitorString.clear();
	}
}

void Server::SendHWInfo()
{
	MakePacketAndBroadcast(en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_CPU_TOTAL, static_cast<int>(_hwMonitor.TotalProcessorTime()));
	MakePacketAndBroadcast(en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NONPAGED_MEMORY, static_cast<int>(_hwMonitor.NonPagedMb()));
	MakePacketAndBroadcast(en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NETWORK_RECV, static_cast<int>(_hwMonitor.RecvKBytes()));
	MakePacketAndBroadcast(en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_NETWORK_SEND, static_cast<int>(_hwMonitor.SendKBytes()));
	MakePacketAndBroadcast(en_SERVER_MONITOR, dfMONITOR_DATA_TYPE_MONITOR_AVAILABLE_MEMORY, static_cast<int>(_hwMonitor.AvailableMb()));
}



void Server::SendChatInfo()
{
	MakePacketAndBroadcast(en_SERVER_CHAT, dfMONITOR_DATA_TYPE_CHAT_SERVER_CPU, static_cast<int>(_chatMonitor.TotalProcessorTime()));
	MakePacketAndBroadcast(en_SERVER_CHAT, dfMONITOR_DATA_TYPE_CHAT_SERVER_MEM, static_cast<int>(_chatMonitor.UseMemoryMB()));
}

void Server::SendLoginInfo()
{
	MakePacketAndBroadcast(en_SERVER_LOGIN, dfMONITOR_DATA_TYPE_LOGIN_SERVER_CPU, static_cast< int >( _loginMonitor.TotalProcessorTime() ));
	MakePacketAndBroadcast(en_SERVER_LOGIN, dfMONITOR_DATA_TYPE_LOGIN_SERVER_MEM, static_cast< int >( _loginMonitor.UseMemoryMB() ));
}

void Server::SendGameInfo()
{
	MakePacketAndBroadcast(en_SERVER_GAME, dfMONITOR_DATA_TYPE_GAME_SERVER_CPU, static_cast< int >( _gameMonitor.TotalProcessorTime() ));
	MakePacketAndBroadcast(en_SERVER_GAME, dfMONITOR_DATA_TYPE_GAME_SERVER_MEM, static_cast< int >( _gameMonitor.UseMemoryMB() ));
}

void Server::SendDBQuery(int group, int serverNo, int dataType, int min, int max, double avg, int count)
{
	thread_local DBConnection conn(_dbIP.c_str(), _dbPort, _dbID.c_str(), _dbPass.c_str(), _basicDatabase.c_str());

	std::time_t now_c = chrono::system_clock::to_time_t(chrono::system_clock::now());
	tm local_time;
	localtime_s(&local_time, &now_c);

	string query = format("INSERT INTO monitorlog_{} "
						  "( `group`, `serverno`, `type`, `min`, `max`, `avg`, `count`)"
						  "VALUES('{}', '{}', '{}', '{}', '{}', '{}', '{}'); ",
						  local_time.tm_mon + 1, group, serverNo, dataType, min, max, avg, count);

	try
	{
		conn.Query(query.c_str());
	}
	catch (DBErr err )
	{
		if ( err.getErrCode() == 1146 )
		{
			conn.Query("CREATE TABLE monitorlog_%d LIKE monitorLog_template", local_time.tm_mon + 1);
			conn.Query(query.c_str());
		}
		else
		{
			throw DBErr(err);
		}
	}
}

void Server::MakePacketAndBroadcast(int serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE dataType, int data)
{
	SharedLockGuard lock(_connectionLock);
	if ( _connections.size() == 0 )
	{
		return;
	}

	auto& buffer = *CSendBuffer::Alloc();
	MakeMonitorPacket(buffer, serverType, dataType, data);
	BroadcastToClient(buffer);
	buffer.Release();
}

void Server::MakeMonitorPacket(CSendBuffer& buffer, BYTE serverType, en_PACKET_SS_MONITOR_DATA_UPDATE_TYPE type, int value)
{
	buffer << en_PACKET_CS_MONITOR_TOOL_DATA_UPDATE << serverType << type << value << int(time(nullptr));
}

void Server::BroadcastToClient(CSendBuffer& sendBuffer)
{
	SharedLockGuard lock(_connectionLock);
	for ( const auto& it : _connections )
	{
		if ( it.second.isClient && it.second.isLogin )
		{
			SendPacket(it.first, &sendBuffer);
		}
	}
}

void Server::OnConnect(SessionID sessionId, const SockAddr_in& info)
{
	if ( _subnetManager.IsLan(info) )
	{
		SetSessionStaticKey(sessionId, 0);

		ExclusiveLockGuard lock(_connectionLock);
		_connections.emplace(sessionId, false);
		return;
	}
	if ( _subnetManager.IsWan(info) )
	{
		ExclusiveLockGuard lock(_connectionLock);
		_connections.emplace(sessionId, true);
		return;
	}

	DisconnectSession(sessionId);
}

