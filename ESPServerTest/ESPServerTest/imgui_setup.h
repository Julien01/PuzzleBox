#pragma once

// Windows (BELANGRIJK voor Winsock conflicts)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Dear ImGui core
#include "imgui.h"

// Backends
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// DirectX
#include <d3d11.h>

// Optioneel (handig voor types)
#include <tchar.h>