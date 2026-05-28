#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>

#include "imgui_setup.h"

#include <d3d11.h>
#include <tchar.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "windowscodecs.lib")

// ------------------------------------------------------------
// DirectX globals
// ------------------------------------------------------------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0;
static UINT g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// ------------------------------------------------------------
// Puzzle image globals
// ------------------------------------------------------------
static ID3D11ShaderResourceView* g_PuzzleTexture = nullptr;
static int g_PuzzleImageWidth = 0;
static int g_PuzzleImageHeight = 0;

// ------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ------------------------------------------------------------
// Network config
// ------------------------------------------------------------
static const int MAX_ESPS = 10;
static const uint16_t UDP_DISCOVERY_PORT = 4444;
static const uint16_t TCP_SERVER_PORT = 3333;
static const unsigned long DEVICE_TIMEOUT_MS = 15000;

// ------------------------------------------------------------
// Commands
// ------------------------------------------------------------
static const int CMD_START_GAME = 100;
static const int CMD_RESET_GAME = 101;
static const int CMD_NEXT_PUZZLE = 69;
static const int CMD_END_GAME = 67;

// ------------------------------------------------------------
// Device model
// ------------------------------------------------------------
struct EspClient
{
    SOCKET socket = INVALID_SOCKET;
    std::string uniqueId;
    std::string displayName;
    std::string ip;
    std::string rxBuffer;
    std::string lastMessage;
    std::string status = "Connected";
    unsigned long long lastSeenTick = 0;
    int numberToSend = 0;
    bool helloReceived = false;

    int activePuzzleNumber = -1;
    int activePuzzleState = -1;

    // individuele planning per puzzle box
    char scheduleInputBuffer[16] = "14:00:00";
    bool scheduleActive = false;
    time_t scheduledStartTime = 0;

    // nieuwe extra tab
    char vaultCodeBuffer[8] = "1234";

    bool gameTimerActive = false;
    time_t gameStartTime = 0;
    bool endSignalSent = false;

    // 13 timer-led stappen
    int timerLedStepSent = 0;

    int batteryPercent = 100;
    unsigned long long lastBatteryTickMs = 0;
};

// ------------------------------------------------------------
// Globals
// ------------------------------------------------------------
static bool gWinsockInitialized = false;
static SOCKET gUdpSocket = INVALID_SOCKET;
static SOCKET gListenSocket = INVALID_SOCKET;
static std::vector<EspClient> gClients;
static std::unordered_map<std::string, int> gDisplayNumbersById;
static int gNextDisplayNumber = 1;
static std::string gServerLog;
static std::string gServerStatus = "Not started";

// geplande start
static int gScheduleHour = 14;
static int gScheduleMinute = 0;
static int gScheduleSecond = 0;
static bool gScheduleActive = false;
static time_t gScheduledStartTime = 0;

// nieuwe UI globals
static bool gShowServerWindow = false;
static char gCustomCommandBuffer[128] = "";
static char gScheduleInputBuffer[16] = "14:00:00";

static float gUiScale = 0.85f;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
template<typename T>
static void SafeRelease(T*& ptr)
{
    if (ptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

static float S(float value)
{
    return value * gUiScale;
}

static void AppendLog(const std::string& text)
{
    gServerLog += text + "\n";

    if (gServerLog.size() > 30000)
        gServerLog.erase(0, gServerLog.size() - 20000);
}

static unsigned long long NowMs()
{
    return GetTickCount64();
}

static std::string SocketAddressToString(const sockaddr_in& addr)
{
    char ipStr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, (void*)&addr.sin_addr, ipStr, sizeof(ipStr));
    return std::string(ipStr);
}

static bool SetNonBlocking(SOCKET s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

static int GetOrAssignDisplayNumber(const std::string& uniqueId)
{
    auto it = gDisplayNumbersById.find(uniqueId);

    if (it != gDisplayNumbersById.end())
        return it->second;

    int assigned = gNextDisplayNumber++;
    gDisplayNumbersById[uniqueId] = assigned;
    return assigned;
}

static std::string MakeDisplayName(const std::string& uniqueId)
{
    int n = GetOrAssignDisplayNumber(uniqueId);
    return "ESP " + std::to_string(n);
}

static std::string FormatTimeT(time_t value)
{
    if (value == 0)
        return "-";

    tm localTm{};
    localtime_s(&localTm, &value);

    char buf[64];
    strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &localTm);
    return std::string(buf);
}

static time_t BuildNextScheduledTime(int hour, int minute, int second)
{
    time_t now = time(nullptr);

    tm localTm{};
    localtime_s(&localTm, &now);

    localTm.tm_hour = hour;
    localTm.tm_min = minute;
    localTm.tm_sec = second;

    time_t target = mktime(&localTm);

    if (target <= now)
        target += 24 * 60 * 60;

    return target;
}

static bool ParseScheduleInput(const char* text, int& hour, int& minute, int& second)
{
    if (!text)
        return false;

    int h = 0;
    int m = 0;
    int s = 0;

    int count = sscanf_s(text, "%d:%d:%d", &h, &m, &s);

    if (count != 3)
        return false;

    if (h < 0 || h > 23)
        return false;

    if (m < 0 || m > 59)
        return false;

    if (s < 0 || s > 59)
        return false;

    hour = h;
    minute = m;
    second = s;
    return true;
}

static const char* GetPuzzleName(int puzzleNumber)
{
    switch (puzzleNumber)
    {
    case 0:
        return "NeoTrellis";

    case 1:
        return "Software";

    case 2:
        return "Hardware";

    case 3:
        return "Kluis";

    default:
        return "Onbekend";
    }
}

static bool ParsePuzzleStateLine(const std::string& line, int& puzzleNumber, int& puzzleState)
{
    int parsedPuzzle = -1;
    int parsedState = -1;

    int count = sscanf_s(
        line.c_str(),
        "PUZZLE:%d,STATE:%d",
        &parsedPuzzle,
        &parsedState
    );

    if (count != 2)
        return false;

    puzzleNumber = parsedPuzzle;
    puzzleState = parsedState;
    return true;
}

static void CloseClient(size_t index, const std::string& reason)
{
    if (index >= gClients.size())
        return;

    auto& c = gClients[index];

    AppendLog(
        "Disconnected: " +
        (c.displayName.empty() ? "(unknown)" : c.displayName) +
        " [" + c.uniqueId + "] - " + reason
    );

    if (c.socket != INVALID_SOCKET)
    {
        closesocket(c.socket);
        c.socket = INVALID_SOCKET;
    }

    c.status = "Disconnected";
    c.lastMessage = reason;
    c.rxBuffer.clear();
    c.helloReceived = !c.uniqueId.empty();
}

static bool SendLine(SOCKET s, const std::string& line)
{
    std::string payload = line + "\n";

    int totalSent = 0;
    int totalSize = (int)payload.size();

    while (totalSent < totalSize)
    {
        int sent = send(
            s,
            payload.c_str() + totalSent,
            totalSize - totalSent,
            0
        );

        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();

            if (err == WSAEWOULDBLOCK)
                continue;

            return false;
        }

        totalSent += sent;
    }

    return true;
}

static void SendTimerLedStepToClient(EspClient& c, int step)
{
    if (c.socket == INVALID_SOCKET)
        return;

    if (step < 0)
        step = 0;

    if (step > 13)
        step = 13;

    std::string message = "LEDSTEP:" + std::to_string(step);

    if (SendLine(c.socket, message))
    {
        AppendLog(
            "Sent to " +
            (c.displayName.empty() ? c.ip : c.displayName) +
            ": " +
            message
        );
    }
    else
    {
        c.status = "LEDSTEP send failed";

        AppendLog(
            "LEDSTEP send failed to " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }
}

// ------------------------------------------------------------
// Texture loading using WIC
// ------------------------------------------------------------
static void ReleasePuzzleTexture()
{
    SafeRelease(g_PuzzleTexture);
    g_PuzzleImageWidth = 0;
    g_PuzzleImageHeight = 0;
}

static bool LoadTextureFromFileW(
    const wchar_t* filename,
    ID3D11ShaderResourceView** out_srv,
    int* out_width,
    int* out_height)
{
    if (!filename || !out_srv || !out_width || !out_height || !g_pd3dDevice)
        return false;

    *out_srv = nullptr;
    *out_width = 0;
    *out_height = 0;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);

    IWICImagingFactory* wicFactory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    bool ok = false;

    do
    {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory)
        );

        if (FAILED(hr))
            break;

        hr = wicFactory->CreateDecoderFromFilename(
            filename,
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );

        if (FAILED(hr))
            break;

        hr = decoder->GetFrame(0, &frame);

        if (FAILED(hr))
            break;

        hr = wicFactory->CreateFormatConverter(&converter);

        if (FAILED(hr))
            break;

        hr = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );

        if (FAILED(hr))
            break;

        UINT width = 0;
        UINT height = 0;

        hr = converter->GetSize(&width, &height);

        if (FAILED(hr))
            break;

        std::vector<unsigned char> imageData((size_t)width * (size_t)height * 4);

        hr = converter->CopyPixels(
            nullptr,
            width * 4,
            (UINT)imageData.size(),
            imageData.data()
        );

        if (FAILED(hr))
            break;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subResource{};
        subResource.pSysMem = imageData.data();
        subResource.SysMemPitch = width * 4;

        hr = g_pd3dDevice->CreateTexture2D(&desc, &subResource, &texture);

        if (FAILED(hr))
            break;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        hr = g_pd3dDevice->CreateShaderResourceView(texture, &srvDesc, &srv);

        if (FAILED(hr))
            break;

        *out_srv = srv;
        *out_width = (int)width;
        *out_height = (int)height;

        srv = nullptr;
        ok = true;

    } while (false);

    SafeRelease(srv);
    SafeRelease(texture);
    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(wicFactory);

    if (shouldUninit)
        CoUninitialize();

    return ok;
}

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH];

    DWORD length = GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH
    );

    if (length == 0 || length == MAX_PATH)
        return L"";

    std::wstring exePath(path);

    size_t lastSlash = exePath.find_last_of(L"\\/");

    if (lastSlash == std::wstring::npos)
        return L"";

    return exePath.substr(0, lastSlash + 1);
}

static void LoadPuzzleTexture()
{
    ReleasePuzzleTexture();

    std::wstring imagePath = GetExeDirectory() + L"puzzlebox.png";

    if (LoadTextureFromFileW(
        imagePath.c_str(),
        &g_PuzzleTexture,
        &g_PuzzleImageWidth,
        &g_PuzzleImageHeight))
    {
        AppendLog("Puzzle image loaded: puzzlebox.png");
    }
    else
    {
        AppendLog("Puzzle image NOT loaded.");
        AppendLog("Expected file next to exe: puzzlebox.png");
    }
}

// ------------------------------------------------------------
// Winsock init / cleanup
// ------------------------------------------------------------
static bool InitWinsock()
{
    if (gWinsockInitialized)
        return true;

    WSADATA wsaData{};

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;

    gWinsockInitialized = true;
    return true;
}

static void CleanupWinsock()
{
    for (auto& c : gClients)
    {
        if (c.socket != INVALID_SOCKET)
            closesocket(c.socket);
    }

    gClients.clear();

    if (gUdpSocket != INVALID_SOCKET)
    {
        closesocket(gUdpSocket);
        gUdpSocket = INVALID_SOCKET;
    }

    if (gListenSocket != INVALID_SOCKET)
    {
        closesocket(gListenSocket);
        gListenSocket = INVALID_SOCKET;
    }

    if (gWinsockInitialized)
    {
        WSACleanup();
        gWinsockInitialized = false;
    }
}

// ------------------------------------------------------------
// Network startup
// ------------------------------------------------------------
static bool StartUdpDiscoveryResponder()
{
    gUdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (gUdpSocket == INVALID_SOCKET)
    {
        gServerStatus = "UDP socket failed";
        return false;
    }

    int yes = 1;

    setsockopt(
        gUdpSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        (const char*)&yes,
        sizeof(yes)
    );

    setsockopt(
        gUdpSocket,
        SOL_SOCKET,
        SO_BROADCAST,
        (const char*)&yes,
        sizeof(yes)
    );

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UDP_DISCOVERY_PORT);

    if (bind(gUdpSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        gServerStatus = "UDP bind failed";
        closesocket(gUdpSocket);
        gUdpSocket = INVALID_SOCKET;
        return false;
    }

    if (!SetNonBlocking(gUdpSocket))
    {
        gServerStatus = "UDP non-blocking failed";
        closesocket(gUdpSocket);
        gUdpSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

static bool StartTcpServer()
{
    gListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (gListenSocket == INVALID_SOCKET)
    {
        gServerStatus = "TCP socket failed";
        return false;
    }

    int yes = 1;

    setsockopt(
        gListenSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        (const char*)&yes,
        sizeof(yes)
    );

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_SERVER_PORT);

    if (bind(gListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        gServerStatus = "TCP bind failed";
        closesocket(gListenSocket);
        gListenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(gListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        gServerStatus = "TCP listen failed";
        closesocket(gListenSocket);
        gListenSocket = INVALID_SOCKET;
        return false;
    }

    if (!SetNonBlocking(gListenSocket))
    {
        gServerStatus = "TCP non-blocking failed";
        closesocket(gListenSocket);
        gListenSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

static bool StartNetworking()
{
    if (!InitWinsock())
    {
        gServerStatus = "WSAStartup failed";
        return false;
    }

    if (!StartUdpDiscoveryResponder())
        return false;

    if (!StartTcpServer())
        return false;

    gServerStatus = "UDP discovery + TCP server running";
    AppendLog("Server started. UDP 4444, TCP 3333");

    return true;
}

// ------------------------------------------------------------
// Discovery handling
// ------------------------------------------------------------
static void PollUdpDiscovery()
{
    if (gUdpSocket == INVALID_SOCKET)
        return;

    for (;;)
    {
        sockaddr_in remoteAddr{};
        int remoteLen = sizeof(remoteAddr);

        char buffer[256];

        int received = recvfrom(
            gUdpSocket,
            buffer,
            sizeof(buffer) - 1,
            0,
            (sockaddr*)&remoteAddr,
            &remoteLen
        );

        if (received == SOCKET_ERROR)
        {
            int err = WSAGetLastError();

            if (err == WSAEWOULDBLOCK)
                break;

            AppendLog("UDP recv error");
            break;
        }

        buffer[received] = '\0';

        std::string msg(buffer);
        std::string ip = SocketAddressToString(remoteAddr);

        if (msg == "DISCOVER_GUI")
        {
            std::string response = "GUI_HERE:" + std::to_string(TCP_SERVER_PORT);

            sendto(
                gUdpSocket,
                response.c_str(),
                (int)response.size(),
                0,
                (sockaddr*)&remoteAddr,
                remoteLen
            );

            AppendLog("Discovery reply sent to " + ip);
        }
    }
}

// ------------------------------------------------------------
// TCP accept
// ------------------------------------------------------------
static void PollAcceptNewClients()
{
    if (gListenSocket == INVALID_SOCKET)
        return;

    for (;;)
    {
        sockaddr_in remoteAddr{};
        int remoteLen = sizeof(remoteAddr);

        SOCKET clientSocket = accept(
            gListenSocket,
            (sockaddr*)&remoteAddr,
            &remoteLen
        );

        if (clientSocket == INVALID_SOCKET)
        {
            int err = WSAGetLastError();

            if (err == WSAEWOULDBLOCK)
                break;

            AppendLog("accept() failed");
            break;
        }

        std::string ip = SocketAddressToString(remoteAddr);

        if ((int)gClients.size() >= MAX_ESPS)
        {
            AppendLog("Client refused, max reached: " + ip);
            closesocket(clientSocket);
            continue;
        }

        if (!SetNonBlocking(clientSocket))
        {
            AppendLog("Client non-blocking failed: " + ip);
            closesocket(clientSocket);
            continue;
        }

        EspClient c;
        c.socket = clientSocket;
        c.ip = ip;
        c.lastSeenTick = NowMs();
        c.status = "Connected, waiting for HELLO";

        gClients.push_back(c);

        AppendLog("TCP client connected from " + ip);
    }
}

// ------------------------------------------------------------
// Message handling
// ------------------------------------------------------------
static void HandleClientLine(EspClient& c, const std::string& line)
{
    c.lastSeenTick = NowMs();
    c.lastMessage = line;

    if (line.rfind("HELLO:", 0) == 0)
    {
        std::string newUniqueId = line.substr(6);

        for (size_t i = 0; i < gClients.size(); ++i)
        {
            EspClient& existing = gClients[i];

            if (&existing == &c)
                continue;

            if (!newUniqueId.empty() && existing.uniqueId == newUniqueId)
            {
                AppendLog(
                    "Reconnect detected for " +
                    existing.displayName +
                    " [" + newUniqueId + "]"
                );

                if (existing.socket != INVALID_SOCKET)
                    closesocket(existing.socket);

                c.uniqueId = existing.uniqueId;
                c.displayName = existing.displayName;
                c.numberToSend = existing.numberToSend;
                c.helloReceived = true;
                c.status = "Connected";
                c.lastSeenTick = NowMs();

                existing.socket = INVALID_SOCKET;
                existing.uniqueId.clear();
                existing.displayName.clear();
                existing.status = "Replaced";
                existing.lastMessage = "Reconnected on new socket";

                AppendLog(
                    "HELLO from " +
                    c.displayName +
                    " [" + c.uniqueId + "] @ " +
                    c.ip
                );

                return;
            }
        }

        c.uniqueId = newUniqueId;
        c.displayName = MakeDisplayName(c.uniqueId);
        c.helloReceived = true;
        c.status = "Connected";

        AppendLog(
            "HELLO from " +
            c.displayName +
            " [" + c.uniqueId + "] @ " +
            c.ip
        );

        return;
    }

    if (line.rfind("ACK:", 0) == 0)
    {
        c.status = "Connected";
        return;
    }

    if (line.rfind("OK:", 0) == 0)
    {
        c.status = "Last send acknowledged";

        AppendLog(
            "Received " +
            line +
            " from " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );

        return;
    }

    int puzzleNumber = -1;
    int puzzleState = -1;

    if (ParsePuzzleStateLine(line, puzzleNumber, puzzleState))
    {
        c.activePuzzleNumber = puzzleNumber;
        c.activePuzzleState = puzzleState;
        c.status = "Connected";

        // Niet elke PUZZLE update loggen, anders spam je de server log.
        return;
    }

    AppendLog(
        "Message from " +
        (c.displayName.empty() ? c.ip : c.displayName) +
        ": " +
        line
    );
}

static void CleanupReplacedClients()
{
    for (int i = (int)gClients.size() - 1; i >= 0; --i)
    {
        if (gClients[(size_t)i].status == "Replaced")
            gClients.erase(gClients.begin() + i);
    }
}

static void PollClientTraffic()
{
    if (gClients.empty())
        return;

    fd_set readSet;
    FD_ZERO(&readSet);

    SOCKET maxSocket = 0;

    for (const auto& c : gClients)
    {
        if (c.socket != INVALID_SOCKET)
        {
            FD_SET(c.socket, &readSet);

            if (c.socket > maxSocket)
                maxSocket = c.socket;
        }
    }

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    int result = select(
        (int)maxSocket + 1,
        &readSet,
        nullptr,
        nullptr,
        &timeout
    );

    if (result == SOCKET_ERROR)
    {
        AppendLog("select() failed");
        return;
    }

    for (int i = (int)gClients.size() - 1; i >= 0; --i)
    {
        EspClient& c = gClients[(size_t)i];

        if (c.socket == INVALID_SOCKET)
            continue;

        if (!FD_ISSET(c.socket, &readSet))
            continue;

        char buffer[512];

        int received = recv(c.socket, buffer, sizeof(buffer), 0);

        if (received == 0)
        {
            CloseClient((size_t)i, "peer disconnected");
            continue;
        }

        if (received == SOCKET_ERROR)
        {
            int err = WSAGetLastError();

            if (err != WSAEWOULDBLOCK)
                CloseClient((size_t)i, "recv failed");

            continue;
        }

        c.rxBuffer.append(buffer, buffer + received);

        size_t pos = 0;

        while ((pos = c.rxBuffer.find('\n')) != std::string::npos)
        {
            std::string line = c.rxBuffer.substr(0, pos);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            c.rxBuffer.erase(0, pos + 1);

            HandleClientLine(c, line);
        }
    }
}

static void UpdateTimeoutStatus()
{
    unsigned long long now = NowMs();

    for (auto& c : gClients)
    {
        if (now - c.lastSeenTick > DEVICE_TIMEOUT_MS)
            c.status = "No ACK / stale";
    }
}

static void SendNumberToClient(EspClient& c)
{
    if (c.socket == INVALID_SOCKET)
        return;

    std::string message = "NUM:" + std::to_string(c.numberToSend);

    if (SendLine(c.socket, message))
    {
        c.status = "Number sent";

        AppendLog(
            "Sent to " +
            (c.displayName.empty() ? c.ip : c.displayName) +
            ": " +
            message
        );
    }
    else
    {
        c.status = "Send failed";

        AppendLog(
            "Send failed to " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }
}

static void SendFixedNumberToClient(EspClient& c, int value)
{
    if (c.socket == INVALID_SOCKET)
        return;

    std::string message = "NUM:" + std::to_string(value);

    if (SendLine(c.socket, message))
    {
        c.status = "Sent fixed command";

        AppendLog(
            "Sent to " +
            (c.displayName.empty() ? c.ip : c.displayName) +
            ": " +
            message
        );
    }
    else
    {
        c.status = "Send failed";

        AppendLog(
            "Send failed to " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }
}

static void SendFixedNumberToAll(int value)
{
    for (auto& c : gClients)
        SendFixedNumberToClient(c, value);
}

static bool IsValidVaultCode(const char* code)
{
    if (!code)
        return false;

    if (strlen(code) != 4)
        return false;

    for (int i = 0; i < 4; ++i)
    {
        if (code[i] < '0' || code[i] > '9')
            return false;
    }

    return true;
}

static void StartPuzzleBox(EspClient& c)
{
    // stuur startcommando naar deze ene puzzle box
    SendFixedNumberToClient(c, CMD_START_GAME);

    // start lokale timer van 1 uur
    c.gameTimerActive = true;
    c.gameStartTime = time(nullptr);
    c.endSignalSent = false;

    // timer-led stappen resetten
    c.timerLedStepSent = 0;
    SendTimerLedStepToClient(c, 0);

    // batterij-simulatie starten
    c.lastBatteryTickMs = NowMs();

    AppendLog(
        "1 hour puzzle timer started for " +
        (c.displayName.empty() ? c.ip : c.displayName)
    );
}

static void StartPuzzleBoxAll()
{
    for (auto& c : gClients)
        StartPuzzleBox(c);
}

static int GetRemainingGameSeconds(const EspClient& c)
{
    if (!c.gameTimerActive || c.gameStartTime == 0)
        return 60 * 60;

    time_t now = time(nullptr);
    int elapsed = (int)difftime(now, c.gameStartTime);
    int remaining = (60 * 60) - elapsed;

    if (remaining < 0)
        remaining = 0;

    return remaining;
}

static std::string FormatRemainingTimer(int seconds)
{
    int minutes = seconds / 60;
    int sec = seconds % 60;

    char buffer[32];
    sprintf_s(buffer, "%02d:%02d", minutes, sec);

    return std::string(buffer);
}

static void UpdateBatterySimulation(EspClient& c)
{
    unsigned long long now = NowMs();

    if (c.lastBatteryTickMs == 0)
    {
        c.lastBatteryTickMs = now;
        return;
    }

    unsigned long long elapsedMs = now - c.lastBatteryTickMs;

    while (elapsedMs >= 60000 && c.batteryPercent > 0)
    {
        c.batteryPercent -= 1;
        c.lastBatteryTickMs += 60000;
        elapsedMs -= 60000;
    }

    if (c.batteryPercent < 0)
        c.batteryPercent = 0;
}

static void SendVaultCodeToClient(EspClient& c)
{
    if (!IsValidVaultCode(c.vaultCodeBuffer))
    {
        AppendLog(
            "Vault code not sent to " +
            (c.displayName.empty() ? c.ip : c.displayName) +
            ": invalid code"
        );

        return;
    }

    std::string message = "VAULT:" + std::string(c.vaultCodeBuffer);

    if (c.socket == INVALID_SOCKET)
        return;

    if (SendLine(c.socket, message))
    {
        c.status = "Vault code sent";

        AppendLog(
            "Sent to " +
            (c.displayName.empty() ? c.ip : c.displayName) +
            ": " +
            message
        );
    }
    else
    {
        c.status = "Vault code send failed";

        AppendLog(
            "Vault code send failed to " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }
}

static void SendRawCommandToAll(const std::string& command)
{
    if (command.empty())
        return;

    for (auto& c : gClients)
    {
        if (c.socket == INVALID_SOCKET)
            continue;

        if (SendLine(c.socket, command))
        {
            c.status = "Custom command sent";

            AppendLog(
                "Custom sent to " +
                (c.displayName.empty() ? c.ip : c.displayName) +
                ": " +
                command
            );
        }
        else
        {
            c.status = "Custom send failed";

            AppendLog(
                "Custom send failed to " +
                (c.displayName.empty() ? c.ip : c.displayName)
            );
        }
    }
}

static void PollScheduledStart()
{
    if (!gScheduleActive || gScheduledStartTime == 0)
        return;

    time_t now = time(nullptr);

    if (now >= gScheduledStartTime)
    {
        AppendLog("Scheduled start triggered at " + FormatTimeT(now));
        StartPuzzleBoxAll();

        gScheduleActive = false;
        gScheduledStartTime = 0;
    }
}

static void PollClientScheduledStarts()
{
    time_t now = time(nullptr);

    for (auto& c : gClients)
    {
        if (!c.scheduleActive || c.scheduledStartTime == 0)
            continue;

        if (now >= c.scheduledStartTime)
        {
            AppendLog(
                "Individual scheduled start triggered for " +
                (c.displayName.empty() ? c.ip : c.displayName) +
                " at " +
                FormatTimeT(now)
            );

            StartPuzzleBox(c);

            c.scheduleActive = false;
            c.scheduledStartTime = 0;
        }
    }
}
static void PollGameTimers()
{
    const int totalGameSeconds = 60 * 60;
    const int totalLedSteps = 13;

    for (auto& c : gClients)
    {
        if (!c.gameTimerActive)
            continue;

        int remainingSeconds = GetRemainingGameSeconds(c);
        int elapsedSeconds = totalGameSeconds - remainingSeconds;

        if (elapsedSeconds < 0)
            elapsedSeconds = 0;

        if (elapsedSeconds > totalGameSeconds)
            elapsedSeconds = totalGameSeconds;

        int currentLedStep = (elapsedSeconds * totalLedSteps) / totalGameSeconds;

        if (currentLedStep < 0)
            currentLedStep = 0;

        if (currentLedStep > totalLedSteps)
            currentLedStep = totalLedSteps;

        // Stuur alleen nieuwe stappen.
        // Als de GUI even laggt, stuurt hij gemiste stappen alsnog op volgorde.
        while (c.timerLedStepSent < currentLedStep)
        {
            c.timerLedStepSent++;
            SendTimerLedStepToClient(c, c.timerLedStepSent);
        }

        // Timer afgelopen: stuur be-eindig signaal eenmalig
        if (remainingSeconds <= 0 && !c.endSignalSent)
        {
            SendFixedNumberToClient(c, CMD_END_GAME);

            c.endSignalSent = true;
            c.gameTimerActive = false;

            AppendLog(
                "Game timer ended. End signal sent to " +
                (c.displayName.empty() ? c.ip : c.displayName)
            );
        }
    }
}

// ------------------------------------------------------------
// UI helpers
// ------------------------------------------------------------
static void DrawPuzzleImage(float maxWidth, float maxHeight)
{
    if (!g_PuzzleTexture || g_PuzzleImageWidth <= 0 || g_PuzzleImageHeight <= 0)
    {
        ImGui::Button("Geen afbeelding geladen", ImVec2(maxWidth, maxHeight));
        return;
    }

    float imgW = (float)g_PuzzleImageWidth;
    float imgH = (float)g_PuzzleImageHeight;

    float scale = std::min(maxWidth / imgW, maxHeight / imgH);

    ImVec2 finalSize(imgW * scale, imgH * scale);

    ImGui::Image((ImTextureID)g_PuzzleTexture, finalSize);
}

static void DrawEspCard(size_t index, float cardHeight)
{
    if (index >= gClients.size())
        return;

    EspClient& c = gClients[index];

    UpdateBatterySimulation(c);

    ImGui::PushID((int)index);

    float fullWidth = ImGui::GetContentRegionAvail().x;

    ImGui::BeginChild(
        "esp_card",
        ImVec2(fullWidth, cardHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float imageWidth = S(450.0f);
    float contentWidth = fullWidth - imageWidth - spacing - 20.0f;

    if (contentWidth < S(1060.0f))
        contentWidth = S(1060.0f);

    // --------------------------------------------------------
    // Linker content
    // --------------------------------------------------------
    ImGui::BeginChild(
        "esp_content",
        ImVec2(contentWidth, cardHeight - S(10.0f)),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    // --------------------------------------------------------
    // Bovenregel: naam links, status + info midden, actieve puzzel rechts
    // --------------------------------------------------------
    ImGui::Text(
        "Naam: %s",
        c.displayName.empty() ? "Waiting for HELLO..." : c.displayName.c_str()
    );

    // Status iets verder naar rechts
    ImGui::SameLine(contentWidth * 0.32f);

    ImGui::Text("Status: %s", c.status.c_str());

    // Info-knop direct naast status
    ImGui::SameLine();

    ImGui::SmallButton("(i)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();

        ImGui::Text(
            "MAC / Unique ID: %s",
            c.uniqueId.empty() ? "-" : c.uniqueId.c_str()
        );

        ImGui::Text(
            "IP: %s",
            c.ip.empty() ? "-" : c.ip.c_str()
        );

        ImGui::Separator();

        ImGui::TextWrapped(
            "Laatste bericht: %s",
            c.lastMessage.empty() ? "-" : c.lastMessage.c_str()
        );

        ImGui::EndTooltip();
    }

    // Actieve puzzel verder naar rechts, met meer ruimte tussen status en actieve puzzel
    ImGui::SameLine(contentWidth * 0.68f);

    if (c.activePuzzleNumber >= 0)
    {
        ImGui::Text(
            "Actieve puzzel: %s",
            GetPuzzleName(c.activePuzzleNumber)
        );
    }
    else
    {
        ImGui::Text("Actieve puzzel: -");
    }

    ImGui::Separator();
    ImGui::Spacing();

    // --------------------------------------------------------
    // Hoofdindeling binnen de kaart
    // --------------------------------------------------------
    float startSectionWidth = S(400.0f);
    float separatorWidth = S(20.0f);
    float actionSectionWidth = S(290.0f);
    float extraTabWidth = S(440.0f);

    // --------------------------------------------------------
    // Linker sectie: starttijd
    // --------------------------------------------------------
    ImGui::BeginChild(
        "start_time_section",
        ImVec2(startSectionWidth, 0),
        false
    );

    ImGui::Text("Puzzle box starten");

    if (ImGui::Button("Start nu", ImVec2(S(220), S(42))))
        StartPuzzleBox(c);

    ImGui::Spacing();

    ImGui::Text("Starttijd instellen");

    ImGui::SetNextItemWidth(S(220.0f));

    ImGui::InputText(
        "##individual_start_time",
        c.scheduleInputBuffer,
        IM_ARRAYSIZE(c.scheduleInputBuffer)
    );

    int previewHour = 0;
    int previewMinute = 0;
    int previewSecond = 0;

    bool validTime = ParseScheduleInput(
        c.scheduleInputBuffer,
        previewHour,
        previewMinute,
        previewSecond
    );

    if (validTime)
    {
        time_t previewTime = BuildNextScheduledTime(
            previewHour,
            previewMinute,
            previewSecond
        );

        ImGui::TextDisabled(
            "Preview: %s",
            FormatTimeT(previewTime).c_str()
        );
    }
    else
    {
        ImGui::TextDisabled("Preview: ongeldig formaat");
        ImGui::TextDisabled("Gebruik bijvoorbeeld 14:30:00");
    }

    if (c.scheduleActive)
    {
        ImGui::TextDisabled(
            "Gepland: %s",
            FormatTimeT(c.scheduledStartTime).c_str()
        );
    }
    else
    {
        ImGui::TextDisabled("Gepland: geen");
    }

    ImGui::Spacing();

    if (ImGui::Button("Plan start", ImVec2(S(150), S(42))))
    {
        if (validTime)
        {
            c.scheduledStartTime = BuildNextScheduledTime(
                previewHour,
                previewMinute,
                previewSecond
            );

            c.scheduleActive = true;

            AppendLog(
                "Scheduled start set for " +
                (c.displayName.empty() ? c.ip : c.displayName) +
                " at " +
                FormatTimeT(c.scheduledStartTime)
            );
        }
        else
        {
            AppendLog(
                "Scheduled start not set for " +
                (c.displayName.empty() ? c.ip : c.displayName) +
                ": invalid time input"
            );
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Annuleer planning", ImVec2(S(230), S(42))))
    {
        c.scheduleActive = false;
        c.scheduledStartTime = 0;

        AppendLog(
            "Scheduled start cancelled for " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Verticale scheidingslijn
    // --------------------------------------------------------
    ImGui::BeginChild(
        "separator_start_actions",
        ImVec2(separatorWidth, 0),
        false
    );

    ImVec2 p1 = ImGui::GetCursorScreenPos();
    ImDrawList* drawList1 = ImGui::GetWindowDrawList();

    drawList1->AddLine(
        ImVec2(p1.x + separatorWidth * 0.5f, p1.y),
        ImVec2(p1.x + separatorWidth * 0.5f, p1.y + cardHeight - S(90.0f)),
        ImGui::GetColorU32(ImGuiCol_Border),
        1.0f
    );

    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Puzzle box acties
    // --------------------------------------------------------
    ImGui::BeginChild(
        "action_section",
        ImVec2(actionSectionWidth, 0),
        false
    );

    ImGui::Text("Puzzle box acties");

    if (ImGui::Button("Volgende puzzel", ImVec2(S(280), S(46))))
        SendFixedNumberToClient(c, CMD_NEXT_PUZZLE);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Reset puzzel doos", ImVec2(S(280), S(46))))
        SendFixedNumberToClient(c, CMD_RESET_GAME);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Be-eindig puzzel doos", ImVec2(S(280), S(46))))
        SendFixedNumberToClient(c, CMD_END_GAME);

    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Verticale scheidingslijn tussen acties en extra tab
    // --------------------------------------------------------
    ImGui::BeginChild(
        "separator_actions_extra",
        ImVec2(separatorWidth, 0),
        false
    );

    ImVec2 p2 = ImGui::GetCursorScreenPos();
    ImDrawList* drawList2 = ImGui::GetWindowDrawList();

    drawList2->AddLine(
        ImVec2(p2.x + separatorWidth * 0.5f, p2.y),
        ImVec2(p2.x + separatorWidth * 0.5f, p2.y + cardHeight - S(90.0f)),
        ImGui::GetColorU32(ImGuiCol_Border),
        1.0f
    );

    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Extra tab: boven kluis/timer, onder batterij
    // --------------------------------------------------------
    ImGui::BeginChild(
        "extra_status_tab",
        ImVec2(extraTabWidth, 0),
        false
    );

    float availableHeight = ImGui::GetContentRegionAvail().y;
    float topHeight = availableHeight * 0.52f;

    // ---------------- Bovenkant ----------------
    ImGui::BeginChild(
        "vault_timer_top",
        ImVec2(0, topHeight),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::Text("Kluiscode");

    ImGui::SetNextItemWidth(S(120.0f));

    ImGui::InputText(
        "##vault_code",
        c.vaultCodeBuffer,
        IM_ARRAYSIZE(c.vaultCodeBuffer),
        ImGuiInputTextFlags_CharsDecimal
    );

    ImGui::SameLine();

    if (ImGui::Button("Stel in", ImVec2(S(110), S(46))))
        SendVaultCodeToClient(c);

    ImGui::SameLine();

    if (!IsValidVaultCode(c.vaultCodeBuffer))
        ImGui::TextDisabled("Code moet 4 cijfers zijn");
    else
        ImGui::TextDisabled("Huidige code: %s", c.vaultCodeBuffer);

    int remainingSeconds = GetRemainingGameSeconds(c);
    std::string timerText = FormatRemainingTimer(remainingSeconds);

    ImGui::Spacing();
    ImGui::Text("Timer: %s", timerText.c_str());

    float timerProgress = (float)remainingSeconds / (60.0f * 60.0f);

    if (timerProgress < 0.0f)
        timerProgress = 0.0f;

    if (timerProgress > 1.0f)
        timerProgress = 1.0f;

    ImGui::ProgressBar(timerProgress, ImVec2(-1, S(36)));

    ImGui::EndChild();

    ImGui::Separator();

    // ---------------- Onderkant ----------------
    ImGui::BeginChild(
        "battery_bottom",
        ImVec2(0, 0),
        false
    );

    ImGui::Text("Batterij");

    float batteryProgress = (float)c.batteryPercent / 100.0f;

    if (batteryProgress < 0.0f)
        batteryProgress = 0.0f;

    if (batteryProgress > 1.0f)
        batteryProgress = 1.0f;

    char batteryText[32];
    sprintf_s(batteryText, "%d%%", c.batteryPercent);

    ImGui::ProgressBar(
        batteryProgress,
        ImVec2(-1, S(36)),
        batteryText
    );

    ImGui::Spacing();

    if (ImGui::Button("Reset batterij", ImVec2(-1, S(38))))
    {
        c.batteryPercent = 100;
        c.lastBatteryTickMs = NowMs();

        AppendLog(
            "Battery reset for " +
            (c.displayName.empty() ? c.ip : c.displayName)
        );
    }

    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Rechter image
    // --------------------------------------------------------
    ImGui::BeginChild(
        "esp_image",
        ImVec2(imageWidth, cardHeight - S(10.0f)),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    DrawPuzzleImage(imageWidth - S(10.0f), cardHeight - S(30.0f));

    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::PopID();
}

static void DrawServerWindow()
{
    if (!gShowServerWindow)
        return;

    ImGui::SetNextWindowSize(ImVec2(S(800), S(520)), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Server log en custom commands", &gShowServerWindow))
    {
        ImGui::Text("Server status: %s", gServerStatus.c_str());
        ImGui::Text("UDP: %d | TCP: %d", UDP_DISCOVERY_PORT, TCP_SERVER_PORT);
        ImGui::Text("Verbonden ESP's: %d / %d", (int)gClients.size(), MAX_ESPS);

        ImGui::Separator();

        ImGui::Text("Custom command naar alle verbonden ESP's");

        ImGui::SetNextItemWidth(-S(150.0f));

        ImGui::InputText(
            "##custom_command",
            gCustomCommandBuffer,
            IM_ARRAYSIZE(gCustomCommandBuffer)
        );

        ImGui::SameLine();

        if (ImGui::Button("Stuur", ImVec2(S(130), 0)))
        {
            std::string command = gCustomCommandBuffer;

            if (!command.empty())
            {
                SendRawCommandToAll(command);
                gCustomCommandBuffer[0] = '\0';
            }
        }

        ImGui::TextDisabled("Voorbeelden: NUM:100, NUM:69, NUM:67");

        ImGui::Separator();

        ImGui::Text("Server log");

        ImGui::BeginChild(
            "server_log_scroll",
            ImVec2(0, 0),
            true,
            ImGuiWindowFlags_HorizontalScrollbar
        );

        ImGui::TextUnformatted(gServerLog.c_str());

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
    }

    ImGui::End();
}

static void RenderMainUI()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("Puzzle Box Hub", nullptr, windowFlags);

    float fullWidth = ImGui::GetContentRegionAvail().x;
    float fullHeight = ImGui::GetContentRegionAvail().y;

    float sideWidth = fullWidth * 0.23f;

    if (sideWidth < S(280.0f))
        sideWidth = S(280.0f);

    if (sideWidth > S(420.0f))
        sideWidth = S(420.0f);

    // --------------------------------------------------------
    // Linker kolom: titel buiten vierkant
    // --------------------------------------------------------
    ImGui::BeginChild(
        "left_column",
        ImVec2(sideWidth, fullHeight),
        false
    );

    ImGui::Text("Puzzle Box Hub");

    ImGui::BeginChild(
        "left_control_panel",
        ImVec2(0, 0),
        true
    );

    // --------------------------------------------------------
    // Sectie 1: server status
    // --------------------------------------------------------
    ImGui::Text("Server status");

    ImGui::SameLine();

    ImGui::SmallButton("(i)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("UDP discovery poort: %d", UDP_DISCOVERY_PORT);
        ImGui::Text("TCP server poort: %d", TCP_SERVER_PORT);
        ImGui::Text("Max ESP's: %d", MAX_ESPS);
        ImGui::Text("Timeout: %lu ms", DEVICE_TIMEOUT_MS);
        ImGui::EndTooltip();
    }

    ImGui::TextWrapped("%s", gServerStatus.c_str());

    ImGui::Text("Verbonden ESP's: %d / %d", (int)gClients.size(), MAX_ESPS);

    ImGui::Separator();

    if (ImGui::Button("Server log / commands", ImVec2(-1, S(42))))
        gShowServerWindow = true;

    ImGui::Separator();

    // --------------------------------------------------------
    // Sectie 2: games starten
    // --------------------------------------------------------
    ImGui::Text("Games starten");

    if (ImGui::Button("Start nu", ImVec2(-1, S(42))))
        StartPuzzleBoxAll();

    ImGui::Spacing();

    ImGui::Text("Starttijd");

    ImGui::SetNextItemWidth(-1);

    ImGui::InputText(
        "##global_schedule_time",
        gScheduleInputBuffer,
        IM_ARRAYSIZE(gScheduleInputBuffer)
    );

    int previewHour = 0;
    int previewMinute = 0;
    int previewSecond = 0;

    bool validTime = ParseScheduleInput(
        gScheduleInputBuffer,
        previewHour,
        previewMinute,
        previewSecond
    );

    if (validTime)
    {
        time_t previewTime = BuildNextScheduledTime(
            previewHour,
            previewMinute,
            previewSecond
        );

        ImGui::TextWrapped(
            "Preview: %s",
            FormatTimeT(previewTime).c_str()
        );
    }
    else
    {
        ImGui::TextWrapped("Preview: ongeldig formaat");
        ImGui::TextDisabled("Gebruik bijvoorbeeld: 14:30:00");
    }

    if (ImGui::Button("Plan start", ImVec2(-1, S(38))))
    {
        if (validTime)
        {
            gScheduleHour = previewHour;
            gScheduleMinute = previewMinute;
            gScheduleSecond = previewSecond;

            gScheduledStartTime = BuildNextScheduledTime(
                gScheduleHour,
                gScheduleMinute,
                gScheduleSecond
            );

            gScheduleActive = true;

            AppendLog(
                "Global scheduled start set for " +
                FormatTimeT(gScheduledStartTime)
            );
        }
        else
        {
            AppendLog("Global scheduled start not set: invalid time input");
        }
    }

    if (ImGui::Button("Annuleer planning", ImVec2(-1, S(38))))
    {
        gScheduleActive = false;
        gScheduledStartTime = 0;

        AppendLog("Global scheduled start cancelled");
    }

    if (gScheduleActive)
    {
        ImGui::TextWrapped(
            "Actief: %s",
            FormatTimeT(gScheduledStartTime).c_str()
        );
    }
    else
    {
        ImGui::TextWrapped("Actief: geen");
    }

    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();

    // --------------------------------------------------------
    // Rechter hoofdgebied
    // --------------------------------------------------------
    ImGui::BeginChild(
        "right_main_panel",
        ImVec2(0, fullHeight),
        false
    );

    ImGui::Text("Individuele puzzle boxes");

    ImGui::BeginChild(
        "esp_scroll_area",
        ImVec2(0, 0),
        true
    );

    if (gClients.empty())
    {
        ImGui::Text("Geen ESP's verbonden.");
    }
    else
    {
        for (size_t i = 0; i < gClients.size(); ++i)
        {
            DrawEspCard(i, S(430.0f));
            ImGui::Spacing();
        }
    }

    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::End();

    DrawServerWindow();
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();

    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)
    );

    WNDCLASSEXW wc =
    {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        L"ESPDynamicServerClass",
        nullptr
    };

    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"ESP Dynamic GUI Server",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        (int)(1400 * main_scale * gUiScale),
        (int)(950 * main_scale * gUiScale),
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    style.ScaleAllSizes(main_scale * gUiScale);

    style.WindowRounding = S(4.0f);
    style.ChildRounding = S(4.0f);
    style.FrameRounding = S(4.0f);
    style.PopupRounding = S(4.0f);
    style.ScrollbarRounding = S(4.0f);
    style.GrabRounding = S(4.0f);

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.17f, 0.17f, 0.19f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);

    colors[ImGuiCol_Border] = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.33f, 0.33f, 0.37f, 1.00f);

    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.63f, 1.00f);

    io.FontGlobalScale = 1.0f;

    ImFont* mainFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf",
        20.0f * main_scale * gUiScale
    );

    if (mainFont == nullptr)
    {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    StartNetworking();
    LoadPuzzleTexture();

    bool done = false;

    while (!done)
    {
        MSG msg;

        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);

            if (msg.message == WM_QUIT)
                done = true;
        }

        if (done)
            break;

        if (
            g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED
            )
        {
            ::Sleep(10);
            continue;
        }

        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();

            g_pSwapChain->ResizeBuffers(
                0,
                g_ResizeWidth,
                g_ResizeHeight,
                DXGI_FORMAT_UNKNOWN,
                0
            );

            g_ResizeWidth = 0;
            g_ResizeHeight = 0;

            CreateRenderTarget();
        }

        PollUdpDiscovery();
        PollAcceptNewClients();
        PollClientTraffic();
        CleanupReplacedClients();
        UpdateTimeoutStatus();
        PollScheduledStart();
        PollClientScheduledStarts();
        PollGameTimers();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderMainUI();

        ImGui::Render();

        const float clear_color_with_alpha[4] =
        {
            0.14f,
            0.14f,
            0.16f,
            1.00f
        };

        g_pd3dDeviceContext->OMSetRenderTargets(
            1,
            &g_mainRenderTargetView,
            nullptr
        );

        g_pd3dDeviceContext->ClearRenderTargetView(
            g_mainRenderTargetView,
            clear_color_with_alpha
        );

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);

        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ReleasePuzzleTexture();
    CleanupWinsock();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();

    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// ------------------------------------------------------------
// DirectX helper functions
// ------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};

    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevel;

    const D3D_FEATURE_LEVEL featureLevelArray[2] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    if (res == DXGI_ERROR_UNSUPPORTED)
    {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext
        );
    }

    if (res != S_OK)
        return false;

    CreateRenderTarget();

    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    SafeRelease(g_pSwapChain);
    SafeRelease(g_pd3dDeviceContext);
    SafeRelease(g_pd3dDevice);
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;

    g_pSwapChain->GetBuffer(
        0,
        IID_PPV_ARGS(&pBackBuffer)
    );

    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(
            pBackBuffer,
            nullptr,
            &g_mainRenderTargetView
        );

        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    SafeRelease(g_mainRenderTargetView);
}

// ------------------------------------------------------------
// Win32 message handler
// ------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;

        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);

        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;

        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}