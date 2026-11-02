# FemboySense

Windows C++ project for CS:GO Legacy, containing the `skeet` DLL, its injector,
and a native Direct3D 9 HLSL backend. This repository contains the native build;
HUD scripts, effect examples and development test harnesses are excluded.

## HLSL implementation

The backend exposes custom shader rendering through the GameSense Lua integration.
It supports pixel and vertex shaders, textures, render targets, geometry,
multipass rendering and optional legacy DirectX effects.

- **API version:** 6.4.1, transport ABI 6.
- **Shader profiles:** `ps_3_0` and `vs_3_0`.
- **Rendering:** Direct3D 9, with device-state preservation and reset handling.
- **Submission:** memory transport, negotiated prepared-frame updates and binary resource uploads.
- **Resource reuse:** cached state blocks, pooled timing queries and reusable geometry buffers.
- **Diagnostics:** optional CPU/GPU timing; profiling is disabled by default.

API version 6 does not mean Shader Model 6. This implementation targets DX9
Shader Model 3 and requires the matching native integration.

## Requirements

- Windows and a Windows 10/11 SDK.
- Visual Studio C++ desktop build tools with MSVC **v145**.
- Internet access for the initial pinned DXSDK header restore, unless cached.

The injector project specifies v143 by default. Either install that toolset too,
or use the v145 override in the build command below, which builds both projects
with the same toolset.

## Build

Run these commands from the repository root in a Visual Studio Developer PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\skeet\hlsl\restore_d3dx.ps1
msbuild .\skeet.sln /m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v145 /p:TreatWarningAsError=true
```

The restore script extracts the required headers from
`Microsoft.DXSDK.D3DX` **9.29.952.8** into `packages/`. It does not install or copy
runtime DLLs. Packages and generated build files are excluded from Git.

Expected output:

```text
Release/
  skeet.dll
  steam.exe
```

**Release/x86** is the supported configuration. Legacy x64 configurations remain
in the solution but are not supported by the Win32 integration. A clean
Release/x86 build has been verified with v145 and warnings treated as errors.

## Injector

Keep `steam.exe` and its matching `skeet.dll` in the same directory. The injector
looks for `csgo.exe` and loads the adjacent DLL. Its executable name does not make
it the Steam client.

Use `-insecure` in the game's launch options. Start with a fresh game process
when changing DLL versions; replacing a file does not update an already loaded
module. Startup diagnostics are written to `loader-startup.log` beside the injector.

To check that the adjacent DLL exists without opening a game process:

```powershell
.\Release\steam.exe --check
```

## Runtime dependencies

Ordinary shader compilation requires the Microsoft `d3dcompiler_47.dll` runtime.
Legacy FX additionally requires `d3dx9_43.dll`. Both are loaded from the Windows
system directory; the header restore step does not provide them.

The native hooks and embedded resources target the supported CS:GO Legacy
client. Compatibility with other game builds is not implied. Build validation
does not establish live FPS or every in-game rendering scenario.

## Source layout

| Path | Purpose |
| --- | --- |
| `skeet/` | Native DLL and integration code. |
| `skeet/hlsl/` | Shader compilation, DX9 rendering, Lua transport and resource management. |
| `skeet/Binary/` | Embedded image, import/export data and required resources. |
| `skeet/VM/` | Required bytecode data and supporting types. |
| `skeet/MinHook/` | Hooking dependency and required library. |
| `skeet/Json/` | JSON dependency. |
| `steam/` | Injector executable source. |

Keep the embedded binary resources, `VM/bytecode.json` and MinHook library:
they are build inputs. `.vs/`, `packages/`, local project settings and generated
outputs should stay outside commits. Preserve the existing third-party copyright
and license notices in bundled dependencies.
