/*
AdbWinGui (Android Debug Bridge Windows GUI)
Copyright (C) 2017  singun

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "AndroidDebugBridge.h"
#include "StringUtils.h"
#include "AndroidEnvVar.h"
#include "DdmPreferences.h"
#include "../System/Process.h"
#include "../System/StreamReader.h"

#define MIN_ADB_VERSION    _T("1.0.20")
#define DDMS				  _T("ddms")
#define DEFAULT_ADB_HOST   _T("127.0.0.1")
#define DEFAULT_ADB_PORT   5037

std::recursive_mutex AndroidDebugBridge::s_lockClass;
std::mutex AndroidDebugBridge::s_lockMember;
AdbVersion* AndroidDebugBridge::s_pCurVersion = NULL;
AndroidDebugBridge* AndroidDebugBridge::s_pThis = NULL;
bool AndroidDebugBridge::s_bInitialized = false;
bool AndroidDebugBridge::s_bClientSupport = false;
int AndroidDebugBridge::s_nAdbServerPort = 0;
SocketAddress AndroidDebugBridge::s_addSocket;

std::set<AndroidDebugBridge::IDebugBridgeChangeListener*> AndroidDebugBridge::s_setBridgeListeners;
std::set<AndroidDebugBridge::IDeviceChangeListener*> AndroidDebugBridge::s_setDeviceListeners;

AndroidDebugBridge::AndroidDebugBridge(const TString szLocation)
{
	m_bStarted = false;
	m_pDeviceMonitor = NULL;
	m_strAdbLocation = szLocation;

	CheckAdbVersion();
}

AndroidDebugBridge::AndroidDebugBridge()
{
	// a bridge not linked to any particular adb executable.
}

AndroidDebugBridge::~AndroidDebugBridge()
{
	Stop();
}

void AndroidDebugBridge::CheckAdbVersion()
{
	// default is bad check
	m_bVersionCheck = false;

	if (m_strAdbLocation.empty())
	{
		return;
	}

	if (s_pCurVersion == NULL)
	{
		s_pCurVersion = AdbVersion::ParseFrom(MIN_ADB_VERSION);
	}
	AdbVersion* pVersion = GetAdbVersion(m_strAdbLocation.c_str());
	if (pVersion != NULL && *pVersion > *s_pCurVersion)
	{
		// version check succeed
		m_bVersionCheck = true;
	}
	else
	{
		if (pVersion == NULL)
		{
			LogE(DDMS, _T("Get adb version failed."));
		}
		else
		{
			LogEEx(DDMS, _T("Required minimum version of adb: %s. Current version is %d.%d.%d"),
				MIN_ADB_VERSION, pVersion->m_nMajor, pVersion->m_nMinor, pVersion->m_nMicro);
		}
	}
	delete pVersion;
	return;
}

AdbVersion* AndroidDebugBridge::GetAdbVersion(const TString adb)
{
	std::packaged_task<AdbVersion*()> taskVer([&adb]() -> AdbVersion*
	{
		Process procAdb(adb);
		procAdb.OpenReadWrite();
		procAdb.Start();
		procAdb.CloseWrite();

		// read process output
		const int BUFFER_SIZE = 1024;
		CharStreamReader frProcess(procAdb.GetRead(), BUFFER_SIZE);
		std::tstringstream& tss = frProcess.ReadData();

		// parse adb version results
		AdbVersion* pVersion = NULL;
		std::tstring line;
		while (std::getline(tss, line))
		{
			StringUtils::TrimString(line);
			pVersion = AdbVersion::ParseFrom(line.c_str());
			if (pVersion != AdbVersion::UNKNOWN)
			{
				break;
			}
		}
		procAdb.CloseRead();

		return pVersion;
	});
	
	AdbVersion* pRet = NULL;
	std::future<AdbVersion*> futureVer = taskVer.get_future();
	std::thread(std::move(taskVer)).detach();
	std::future_status status = futureVer.wait_for(std::chrono::seconds(5));
	if (status == std::future_status::ready)
	{
		pRet = futureVer.get();
	}
	else
	{
		LogE(DDMS, _T("Unable to obtain result of 'adb version'"));
	}
	return pRet;
}

AndroidDebugBridge& AndroidDebugBridge::CreateBridge(const TString szLocation, bool forceNewBridge)
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	if (s_pThis != NULL)
	{
		if (!s_pThis->m_strAdbLocation.empty() && s_pThis->m_strAdbLocation.compare(szLocation) == 0 &&
			!forceNewBridge)
		{
			return *s_pThis;
		}
		else
		{
			// stop the current server
			s_pThis->Stop();
		}
	}
	s_pThis = new AndroidDebugBridge(szLocation);
	s_pThis->Start();

	// because the listeners could remove themselves from the list while processing
	// their event callback, we make a copy of the list and iterate on it instead of
	// the main list.
	// This mostly happens when the application quits.
	if (s_setBridgeListeners.size() > 0)
	{
		std::vector<IDebugBridgeChangeListener*> vecListener;
		vecListener.assign(s_setBridgeListeners.begin(), s_setBridgeListeners.end());

		// Notify the listeners
		for (IDebugBridgeChangeListener* listener : vecListener)
		{
			listener->BridgeChanged(s_pThis);
		}
	}

	return *s_pThis;
}

AndroidDebugBridge& AndroidDebugBridge::CreateBridge()
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	if (s_pThis != NULL)
	{
		return *s_pThis;
	}

	s_pThis = new AndroidDebugBridge();
	s_pThis->Start();

	// because the listeners could remove themselves from the list while processing
	// their event callback, we make a copy of the list and iterate on it instead of
	// the main list.
	// This mostly happens when the application quits.
	if (s_setBridgeListeners.size() > 0)
	{
		std::vector<IDebugBridgeChangeListener*> vecListener;
		vecListener.assign(s_setBridgeListeners.begin(), s_setBridgeListeners.end());

		// Notify the listeners
		for (IDebugBridgeChangeListener* listener : vecListener)
		{
			listener->BridgeChanged(s_pThis);
		}
	}

	return *s_pThis;
}

void AndroidDebugBridge::DisconnectBridge()
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	if (s_pThis != NULL)
	{
		s_pThis->Stop();
		delete s_pThis;
		s_pThis = NULL;
	}
}

AndroidDebugBridge& AndroidDebugBridge::GetBridge()
{
	return *s_pThis;
}

void AndroidDebugBridge::AddDebugBridgeChangeListener(IDebugBridgeChangeListener* listener)
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	s_setBridgeListeners.insert(listener);
}

void AndroidDebugBridge::RemoveDebugBridgeChangeListener(IDebugBridgeChangeListener* listener)
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	s_setBridgeListeners.erase(listener);
}

void AndroidDebugBridge::AddDeviceChangeListener(IDeviceChangeListener* listener)
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	s_setDeviceListeners.insert(listener);
}

void AndroidDebugBridge::RemoveDeviceChangeListener(IDeviceChangeListener* listener)
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	s_setDeviceListeners.erase(listener);
}

bool AndroidDebugBridge::GetClientSupport()
{
	return s_bClientSupport;
}

const SocketAddress& AndroidDebugBridge::GetSocketAddress()
{
	return s_addSocket;
}

bool AndroidDebugBridge::InitIfNeeded(bool clientSupport)
{
	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	if (s_bInitialized)
	{
		return true;
	}

	return Init(clientSupport);
}

bool AndroidDebugBridge::Init(bool clientSupport)
{
	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	if (s_bInitialized)
	{
		// has already been called
		return false;
	}
	s_bInitialized = true;
	s_bClientSupport = clientSupport;

	// Determine port and instantiate socket address.
	InitAdbSocketAddr();

	return true;
}

void AndroidDebugBridge::InitAdbSocketAddr()
{
	s_nAdbServerPort = GetAdbServerPort();
	s_addSocket.SetSocketAddress(DEFAULT_ADB_HOST);
	s_addSocket.SetSocketPort(s_nAdbServerPort);
}

void AndroidDebugBridge::Terminate()
{
	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	// kill the monitoring services
	if (s_pThis != NULL && s_pThis->m_pDeviceMonitor != NULL)
	{
		s_pThis->m_pDeviceMonitor->Stop();
		delete s_pThis->m_pDeviceMonitor;
		s_pThis->m_pDeviceMonitor = NULL;
	}

	s_bInitialized = false;

	DeviceMonitor::ReleaseConnection();
}

int AndroidDebugBridge::GetAdbServerPort()
{
	AndroidEnvVar envVar;
	int nPort = envVar.GetAdbServerPort();
	if (nPort <= 0 || nPort >= 65535)
	{
		// invalid port, use default
		return DEFAULT_ADB_PORT;
	}
	return nPort;
}

const IDevice* AndroidDebugBridge::GetDevices() const
{
	std::unique_lock<std::mutex> lock(s_lockMember);
	return NULL;
}

bool AndroidDebugBridge::Start()
{
	if (!m_strAdbLocation.empty() && s_nAdbServerPort != 0 && (!m_bVersionCheck || !StartAdb()))
	{
		return false;
	}
	m_bStarted = true;

	// now that the bridge is connected, we start the underlying services.
	m_pDeviceMonitor = new DeviceMonitor(this);
	m_pDeviceMonitor->Start();
	return true;
}

bool AndroidDebugBridge::Stop()
{
	if (!m_bStarted)
	{
		return false;
	}
	// kill the monitoring services
	if (m_pDeviceMonitor != NULL)
	{
		m_pDeviceMonitor->Stop();
		delete m_pDeviceMonitor;
		m_pDeviceMonitor = NULL;
	}

	if (!StopAdb())
	{
		return false;
	}

	m_bStarted = false;
	return true;
}

bool AndroidDebugBridge::Restart()
{
	if (m_strAdbLocation.empty())
	{
		LogE(DDMS, _T("Cannot restart adb when AndroidDebugBridge is created without the location of adb."));
		return false;
	}

	if (s_nAdbServerPort == 0)
	{
		LogE(DDMS, _T("ADB server port for restarting AndroidDebugBridge is not set."));
		return false;
	}

	if (!m_bVersionCheck)
	{
		LogE(DDMS, _T("Attempting to restart adb, but version check failed!"));
		return false;
	}

	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	StopAdb();

	bool restart = StartAdb();

	if (restart && m_pDeviceMonitor == NULL)
	{
		m_pDeviceMonitor = new DeviceMonitor(this);
		m_pDeviceMonitor->Start();
	}

	return restart;
}

void AndroidDebugBridge::DeviceConnected(const IDevice* device)
{
	s_lockMember.lock();
	if (s_setDeviceListeners.size() == 0)
	{
		s_lockMember.unlock();
		return;
	}
	// because the listeners could remove themselves from the list while processing
	// their event callback, we make a copy of the list and iterate on it instead of
	// the main list.
	// This mostly happens when the application quits.
	std::vector<IDeviceChangeListener*> vecListener;
	vecListener.assign(s_setDeviceListeners.begin(), s_setDeviceListeners.end());
	s_lockMember.unlock();

	// Notify the listeners
	for (IDeviceChangeListener* listener : vecListener)
	{
		listener->DeviceConnected(device);
	}
}

void AndroidDebugBridge::DeviceDisconnected(const IDevice* device)
{
	s_lockMember.lock();
	if (s_setDeviceListeners.size() == 0)
	{
		s_lockMember.unlock();
		return;
	}
	// because the listeners could remove themselves from the list while processing
	// their event callback, we make a copy of the list and iterate on it instead of
	// the main list.
	// This mostly happens when the application quits.
	std::vector<IDeviceChangeListener*> vecListener;
	vecListener.assign(s_setDeviceListeners.begin(), s_setDeviceListeners.end());
	s_lockMember.unlock();

	// Notify the listeners
	for (IDeviceChangeListener* listener : vecListener)
	{
		listener->DeviceDisconnected(device);
	}
}

void AndroidDebugBridge::DeviceChanged(const IDevice* device, int changeMask)
{
	s_lockMember.lock();
	if (s_setDeviceListeners.size() == 0)
	{
		s_lockMember.unlock();
		return;
	}
	// because the listeners could remove themselves from the list while processing
	// their event callback, we make a copy of the list and iterate on it instead of
	// the main list.
	// This mostly happens when the application quits.
	std::vector<IDeviceChangeListener*> vecListener;
	vecListener.assign(s_setDeviceListeners.begin(), s_setDeviceListeners.end());
	s_lockMember.unlock();

	// Notify the listeners
	for (IDeviceChangeListener* listener : vecListener)
	{
		listener->DeviceChanged(device, changeMask);
	}
}

bool AndroidDebugBridge::StartAdb()
{
	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	if (m_strAdbLocation.empty())
	{
		LogE(DDMS, _T("Cannot start adb when AndroidDebugBridge is created without the location of adb."));
		return false;
	}
	if (s_nAdbServerPort == 0)
	{
		LogE(DDMS, _T("ADB server port for starting AndroidDebugBridge is not set."));
		return false;
	}
	std::vector<std::tstring> vecCommand;
	GetAdbLaunchCommand(_T("start-server"), vecCommand);
	Process procServer(vecCommand);
	if (DdmPreferences::GetUseAdbHost())
	{
		const TString adbHostValue = DdmPreferences::GetAdbHostValue();
		if (adbHostValue != NULL && _tcslen(adbHostValue) != 0)
		{
			//TODO : check that the String is a valid IP address
// 			Map<String, String> env = processBuilder.environment();
// 			env.put("ADBHOST", adbHostValue);
		}
	}
	procServer.OpenReadWrite();
	procServer.Start();
	procServer.CloseWrite();

	int status = GrabProcessOutput(procServer, NULL);
	if (status != 0)
	{
		LogEEx(DDMS, _T("'%s' failed -- run manually if necessary"), procServer.GetCmdLine());
		return false;
	}
	else
	{
		LogDEx(DDMS, _T("'%s' succeeded"), procServer.GetCmdLine());
		return true;
	}
}

void AndroidDebugBridge::GetAdbLaunchCommand(const TString option, std::vector<std::tstring>& vecCommand)
{
	vecCommand.clear();
	vecCommand.push_back(m_strAdbLocation.c_str());
	if (s_nAdbServerPort != DEFAULT_ADB_PORT)
	{
		vecCommand.push_back(_T("-P"));
		std::tstringstream tss;
		tss << s_nAdbServerPort;
		vecCommand.push_back(tss.str());
	}
	vecCommand.push_back(option);
}

int AndroidDebugBridge::GrabProcessOutput(Process& process, std::vector<std::tstring>* pOutput)
{
	bool waitForReaders = (pOutput != NULL);
	if (waitForReaders)
	{
		std::thread tdReadOutput([&process, &pOutput]
		{
			// read process output
			const int BUFFER_SIZE = 1024;
			CharStreamReader frProcess(process.GetRead(), BUFFER_SIZE);
			std::tstringstream& tss = frProcess.ReadData();

			std::tstring line;
			while (std::getline(tss, line))
			{
				StringUtils::TrimString(line);
				(*pOutput).push_back(line);
			}
			process.CloseRead();
		});
		tdReadOutput.join();
	}
	DWORD dwRet = process.Join();
	return static_cast<int>(dwRet);
}

bool AndroidDebugBridge::StopAdb()
{
	std::unique_lock<std::recursive_mutex> lock(s_lockClass);
	if (m_strAdbLocation.empty())
	{
		LogE(DDMS, _T("Cannot stop adb when AndroidDebugBridge is created without the location of adb."));
		return false;
	}

	if (s_nAdbServerPort == 0)
	{
		LogE(DDMS, _T("ADB server port for restarting AndroidDebugBridge is not set"));
		return false;
	}

	std::vector<std::tstring> vecCommand;
	GetAdbLaunchCommand(_T("kill-server"), vecCommand);
	Process procServer(vecCommand);
	procServer.Start();

	int status = static_cast<int>(procServer.Join());

	if (status != 0)
	{
		LogWEx(DDMS, _T("'%s' failed -- run manually if necessary"), procServer.GetCmdLine());
		return false;
	}
	else
	{
		LogDEx(DDMS, _T("'%s' succeeded"), procServer.GetCmdLine());
		return true;
	}
}
