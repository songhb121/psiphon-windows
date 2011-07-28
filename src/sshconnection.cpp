/*
 * Copyright (c) 2011, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include <WinSock2.h>
#include <WinCrypt.h>
#include "sshconnection.h"
#include "psiclient.h"
#include "config.h"

SSHConnection::SSHConnection(const bool& cancel)
:   m_cancel(cancel)
{
    ZeroMemory(&m_plinkProcessInfo, sizeof(m_plinkProcessInfo));
    ZeroMemory(&m_polipoProcessInfo, sizeof(m_polipoProcessInfo));
}

SSHConnection::~SSHConnection(void)
{
    Disconnect();
}

extern HINSTANCE hInst;

bool ExtractExecutable(DWORD resourceID, tstring& path)
{
    // Extract executable from resources and write to temporary file

    HRSRC res;
    HGLOBAL handle = INVALID_HANDLE_VALUE;
    BYTE* data;
    DWORD size;

    res = FindResource(hInst, MAKEINTRESOURCE(resourceID), RT_RCDATA);
    if (!res)
    {
        my_print(false, _T("ExtractExecutable - FindResource failed (%d)"), GetLastError());
        return false;
    }

    handle = LoadResource(NULL, res);
    if (!handle)
    {
        my_print(false, _T("ExtractExecutable - LoadResource failed (%d)"), GetLastError());
        return false;
    }

    data = (BYTE*)LockResource(handle);
    size = SizeofResource(NULL, res);

    DWORD ret;
    TCHAR tempPath[MAX_PATH];
    // http://msdn.microsoft.com/en-us/library/aa364991%28v=vs.85%29.aspx notes
    // tempPath can contain no more than MAX_PATH-14 characters
    ret = GetTempPath(MAX_PATH, tempPath);
    if (ret > MAX_PATH-14 || ret == 0)
    {
        my_print(false, _T("ExtractExecutable - GetTempPath failed (%d)"), GetLastError());
        return false;
    }

    TCHAR tempFileName[MAX_PATH];
    ret = GetTempFileName(tempPath, _T(""), 0, tempFileName);
    if (ret == 0)
    {
        my_print(false, _T("ExtractExecutable - GetTempFileName failed (%d)"), GetLastError());
        return false;
    }

    HANDLE tempFile = CreateFile(tempFileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (tempFile == INVALID_HANDLE_VALUE) 
    { 
        my_print(false, _T("ExtractExecutable - CreateFile failed (%d)"), GetLastError());
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(tempFile, data, size, &written, NULL)
        || written != size
        || !FlushFileBuffers(tempFile))
    {
        CloseHandle(tempFile);
        my_print(false, _T("ExtractExecutable - WriteFile/FlushFileBuffers failed (%d)"), GetLastError());
        return false;
    }

    CloseHandle(tempFile);

    path = tempFileName;

    return true;
}

bool SetPlinkSSHHostKey(
        const tstring& sshServerAddress,
        const tstring& sshServerPort,
        const tstring& sshServerPublicKey)
{
    // Add Plink registry entry for host for non-interactive public key validation

    // Host key is base64 encoded set of fiels

    BYTE* decodedFields = NULL;
    DWORD size = 0;

    if (!CryptStringToBinary(sshServerPublicKey.c_str(), sshServerPublicKey.length(), CRYPT_STRING_BASE64, NULL, &size, NULL, NULL)
        || !(decodedFields = new (std::nothrow) BYTE[size])
        || !CryptStringToBinary(sshServerPublicKey.c_str(), sshServerPublicKey.length(), CRYPT_STRING_BASE64, decodedFields, &size, NULL, NULL))
    {
        my_print(false, _T("SetPlinkSSHHostKey: CryptStringToBinary failed (%d)"), GetLastError());
        return false;
    }

    // field format: {<4 byte len (big endian), len bytes field>}+
    // first field is key type, expecting "ssh-rsa";
    // remaining fields are opaque number value -- simply emit in new format which is comma delimited hex strings

    const char* expectedKeyTypeValue = "ssh-rsa";
    unsigned long expectedKeyTypeLen = htonl(strlen(expectedKeyTypeValue));

    if (memcmp(decodedFields + 0, &expectedKeyTypeLen, sizeof(unsigned long))
        || memcmp(decodedFields + sizeof(unsigned long), expectedKeyTypeValue, strlen(expectedKeyTypeValue)))
    {
        delete [] decodedFields;

        my_print(false, _T("SetPlinkSSHHostKey: unexpected key type"));
        return false;
    }

    string data;

    unsigned long offset = sizeof(unsigned long) + strlen(expectedKeyTypeValue);

    while (offset < size - sizeof(unsigned long))
    {
        unsigned long nextLen = ntohl(*((long*)(decodedFields + offset)));
        offset += sizeof(unsigned long);

        if (nextLen > 0 && offset + nextLen <= size)        
        {
            if (data.length() > 0)
            {
                data += ",";
            }
            data += "0x";
            const char* hexDigits = "0123456789abcdef";
            bool leadingZero = true;
            for (unsigned long i = 0; i < nextLen; i++)
            {
                if (0 != decodedFields[offset + i])
                {
                    leadingZero = false;
                }
                else if (leadingZero)
                {
                    continue;
                }
                data += hexDigits[decodedFields[offset + i] >> 4];
                data += hexDigits[decodedFields[offset + i] & 0x0F];
            }
            offset += nextLen;
        }
    }

    delete [] decodedFields;

    string value = string("rsa2@") + TStringToNarrow(sshServerPort) + ":" + TStringToNarrow(sshServerAddress);

    const TCHAR* plinkRegistryKey = _T("Software\\SimonTatham\\PuTTY\\SshHostKeys");

    HKEY key = 0;
    LONG returnCode = RegCreateKeyEx(HKEY_CURRENT_USER, plinkRegistryKey, 0, 0, 0, KEY_WRITE, 0, &key, NULL);
    if (ERROR_SUCCESS != returnCode)
    {
        my_print(false, _T("SetPlinkSSHHostKey: Create Registry Key failed (%d)"), returnCode);
        return false;
    }

    returnCode = RegSetValueExA(key, value.c_str(), 0, REG_SZ, (PBYTE)data.c_str(), data.length()+1);
    if (ERROR_SUCCESS != returnCode)
    {
        RegCloseKey(key);

        my_print(false, _T("SetPlinkSSHHostKey: Set Registry Value failed (%d)"), returnCode);
        return false;
    }

    RegCloseKey(key);

    return true;
}

bool SSHConnection::Connect(
        const tstring& sshServerAddress,
        const tstring& sshServerPort,
        const tstring& sshServerPublicKey,
        const tstring& sshUsername,
        const tstring& sshPassword)
{
    my_print(false, _T("SSH connecting..."));

    // Extract executables and put to disk if not already

    if (m_plinkPath.size() == 0)
    {
        if (!ExtractExecutable(IDR_PLINK_EXE, m_plinkPath))
        {
            return false;
        }
    }

    if (m_polipoPath.size() == 0)
    {
        if (!ExtractExecutable(IDR_POLIPO_EXE, m_polipoPath))
        {
            return false;
        }
    }

    // Ensure we start from a disconnected/clean state

    Disconnect();

    // Add host to Plink's known host registry set
    // Note: currently we're not removing this after the session, so we're leaving a trace

    SetPlinkSSHHostKey(sshServerAddress, sshServerPort, sshServerPublicKey);

    // Start plink using Psiphon server SSH parameters

    // Note: -batch ensures plink doesn't hang on a prompt when the server's host key isn't
    // the expected value we just set in the registry

    tstring plinkCommandLine = m_plinkPath
                               + _T(" -ssh -C -N -batch")
                               + _T(" -P ") + sshServerPort
                               + _T(" -l ") + sshUsername
                               + _T(" -pw ") + sshPassword
                               + _T(" -D ") + PLINK_SOCKS_PROXY_PORT
                               + _T(" ") + sshServerAddress;

    STARTUPINFO plinkStartupInfo;
    ZeroMemory(&plinkStartupInfo, sizeof(plinkStartupInfo));
    plinkStartupInfo.cb = sizeof(plinkStartupInfo);

    if (!CreateProcess(
            m_plinkPath.c_str(),
            (TCHAR*)plinkCommandLine.c_str(),
            NULL,
            NULL,
            FALSE,
#ifdef _DEBUG
            CREATE_NEW_PROCESS_GROUP,
#else
            CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW,
#endif
            NULL,
            NULL,
            &plinkStartupInfo,
            &m_plinkProcessInfo))
    {
        my_print(false, _T("SSHConnection::Connect - Plink CreateProcess failed (%d)"), GetLastError());
        return false;
    }

    // TODO: wait for parent proxy to be in place? See comment in WaitForConnected for
    // various options; in testing, we found cases where Polipo stopped responding
    // when the ssh tunnel was torn down.

    // Start polipo, using plink's SOCKS proxy, with no disk cache and no web admin interface
    // (same recommended settings as Tor: http://www.pps.jussieu.fr/~jch/software/polipo/tor.html

    tstring polipoCommandLine = m_polipoPath
                                + _T(" proxyPort=") + POLIPO_HTTP_PROXY_PORT
                                + _T(" socksParentProxy=127.0.0.1:") + PLINK_SOCKS_PROXY_PORT
                                + _T(" diskCacheRoot=\"\"")
                                + _T(" disableLocalInterface=true")
                                + _T(" logLevel=1");

    STARTUPINFO polipoStartupInfo;
    ZeroMemory(&polipoStartupInfo, sizeof(polipoStartupInfo));
    polipoStartupInfo.cb = sizeof(polipoStartupInfo);

    if (!CreateProcess(
            m_polipoPath.c_str(),
            (TCHAR*)polipoCommandLine.c_str(),
            NULL,
            NULL,
            FALSE,
#ifdef _DEBUG
            CREATE_NEW_PROCESS_GROUP,
#else
            CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW,
#endif
            NULL,
            NULL,
            &polipoStartupInfo,
            &m_polipoProcessInfo))
    {
        my_print(false, _T("SSHConnection::Connect - Polipo CreateProcess failed (%d)"), GetLastError());
        return false;
    }

    return true;
}

void SSHConnection::Disconnect(void)
{
    SignalDisconnect();
    WaitAndDisconnect();
}

bool SSHConnection::WaitForConnected(void)
{
    // There are a number of options for monitoring the connected status
    // of plink/polipo. We're going with a quick and dirty solution of
    // (a) monitoring the child processes -- if they exit, there was an error;
    // (b) asynchronously connecting to the plink SOCKS server, which isn't
    //     started by plink until its ssh tunnel is established.
    // Note: piping stdout/stderr of the child processes and monitoring
    // messages is problematic because we don't control the C I/O flushing
    // of these processes (http://support.microsoft.com/kb/190351).
    // Additional measures or alternatives include making actual HTTP
    // requests through the entire stack from time to time or switching
    // to integrated ssh/http libraries with APIs.

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    sockaddr_in plinkSocksServer;
    plinkSocksServer.sin_family = AF_INET;
    plinkSocksServer.sin_addr.s_addr = inet_addr("127.0.0.1");
    plinkSocksServer.sin_port = htons(atoi(TStringToNarrow(PLINK_SOCKS_PROXY_PORT).c_str()));

    SOCKET sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    WSAEVENT connectedEvent = WSACreateEvent();

    bool connected = false;

    if (0 == WSAEventSelect(sock, connectedEvent, FD_CONNECT)
        && SOCKET_ERROR == connect(sock, (SOCKADDR*)&plinkSocksServer, sizeof(plinkSocksServer))
        && WSAEWOULDBLOCK == WSAGetLastError())
    {
        // Wait up to 10 seconds, checking periodically for user cancel

        for (int i = 0; i < 100; i++)
        {
            if (WSA_WAIT_EVENT_0 == WSAWaitForMultipleEvents(1, &connectedEvent, TRUE, 100, FALSE))
            {
                connected = true;
                break;
            }
            else if (m_cancel)
            {
                break;
            }
        }
    }

    closesocket(sock);
    WSACleanup();

    if (connected)
    {
        // Now that we are connected, change the Windows Internet Settings
        // to use our HTTP proxy

        m_systemProxySettings.Configure();

        my_print(false, _T("SSH successfully connected."));
    }

    return connected;
}

void SSHConnection::WaitAndDisconnect(void)
{
    // See comment in WaitForConnected

    // Wait for either process to terminate, then clean up both
    // If the user cancels manually, m_cancel will be set -- we
    // handle that here while for VPN it's done in Manager

    bool wasConnected = false;

    while (m_plinkProcessInfo.hProcess != 0 && m_polipoProcessInfo.hProcess != 0)
    {
        wasConnected = true;

        HANDLE processes[2];
        processes[0] = m_plinkProcessInfo.hProcess;
        processes[1] = m_polipoProcessInfo.hProcess;

        DWORD result = WaitForMultipleObjects(2, processes, FALSE, 100);

        if (m_cancel || result != WAIT_TIMEOUT)
        {
            break;
        }
    }

    // Attempt graceful shutdown (for the case where one process
    // terminated unexpectedly, not a user cancel)
    SignalDisconnect();

    CloseHandle(m_plinkProcessInfo.hProcess);
    CloseHandle(m_plinkProcessInfo.hThread);
    ZeroMemory(&m_plinkProcessInfo, sizeof(m_plinkProcessInfo));

    CloseHandle(m_polipoProcessInfo.hProcess);
    CloseHandle(m_polipoProcessInfo.hThread);
    ZeroMemory(&m_polipoProcessInfo, sizeof(m_polipoProcessInfo));

    // Revert the Windows Internet Settings to the user's previous settings
    m_systemProxySettings.Revert();

    if (wasConnected)
    {
        my_print(false, _T("SSH disconnected."));
    }
}

void SSHConnection::SignalDisconnect(void)
{
    // Give each process an opportunity for graceful shutdown, then terminate

    if (m_plinkProcessInfo.hProcess != 0)
    {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_plinkProcessInfo.dwProcessId);
        Sleep(100);
        TerminateProcess(m_plinkProcessInfo.hProcess, 0);
    }

    if (m_polipoProcessInfo.hProcess != 0)
    {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_polipoProcessInfo.dwProcessId);
        Sleep(100);
        TerminateProcess(m_polipoProcessInfo.hProcess, 0);
    }
}