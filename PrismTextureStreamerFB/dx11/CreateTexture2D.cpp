#include "dx11.h"
#include <d3d11.h>

#include <MinHook/MinHook.h>

#include <set>

#include "../scs_logging.h"
using namespace scs_logging;

#include "../screens.h"


typedef HRESULT(__stdcall* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t CreateTexture2D_Original = nullptr;

HRESULT HookedCreateTexture2D(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    if (!g_screen_source_creation_in_progress.load()) {
        std::lock_guard<std::mutex> lock(g_screens_mutex);

        for (screen_t& screen : g_screens)
        {
            if (!screen.source.get()) continue; // no source, cant use this

            if (!pDesc) continue;
            if (pDesc->Width != 64 && pDesc->Height != 64 &&
                pDesc->Width != 2048 && pDesc->Height != 2048)
                continue;

            scs_log(0, "[C2D] candidate %ux%u Format=%u Usage=%u BindFlags=0x%X CPUAccessFlags=0x%X MiscFlags=0x%X MipLevels=%u ArraySize=%u InitialData=%s",
                pDesc->Width, pDesc->Height, pDesc->Format, pDesc->Usage, pDesc->BindFlags,
                pDesc->CPUAccessFlags, pDesc->MiscFlags, pDesc->MipLevels, pDesc->ArraySize,
                pInitialData ? "not null" : "null");

            if (pDesc->Width != screen.override_texture_size_w) {
                scs_log(0, "[C2D] reject: Width expected %u", screen.override_texture_size_w);
                continue;
            }
            if (pDesc->Height != screen.override_texture_size_h) {
                scs_log(0, "[C2D] reject: Height expected %u", screen.override_texture_size_h);
                continue;
            }
            if (pDesc->Format != DXGI_FORMAT_BC3_UNORM) {
                scs_log(0, "[C2D] reject: Format expected %u", DXGI_FORMAT_BC3_UNORM);
                continue;
            }
            if (pDesc->Usage != D3D11_USAGE_DEFAULT) {
                scs_log(0, "[C2D] reject: Usage expected %u", D3D11_USAGE_DEFAULT);
                continue;
            }
            if (pDesc->BindFlags != D3D11_BIND_SHADER_RESOURCE) {
                scs_log(0, "[C2D] reject: BindFlags expected 0x%X", D3D11_BIND_SHADER_RESOURCE);
                continue;
            }
            if (pInitialData) {
                scs_log(0, "[C2D] reject: InitialData not null");
                continue;
            }
            if (pDesc->MipLevels != 1) {
                scs_log(0, "[C2D] reject: MipLevels expected 1");
                continue;
            }

            scs_log(0, "[C2D] fingerprint matched");
            D3D11_TEXTURE2D_DESC modifiedDesc = *pDesc;
            modifiedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            modifiedDesc.Usage = D3D11_USAGE_DYNAMIC;
            modifiedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            modifiedDesc.MiscFlags = 0;
            modifiedDesc.Width = screen.targetLiveTextureWidth;
            modifiedDesc.Height = screen.targetLiveTextureHeight;

            HRESULT hr = CreateTexture2D_Original(pDevice, &modifiedDesc, pInitialData, ppTexture2D);
            scs_log(0, "[C2D] original CreateTexture2D HRESULT=0x%08X returned texture ptr=%p",
                hr, (ppTexture2D ? *ppTexture2D : nullptr));
            if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D)
            {
                if (screen.liveTexture) screen.liveTexture->Release();
                if (screen.immediateContext) screen.immediateContext->Release();

                screen.liveTextureWidth = modifiedDesc.Width;
                screen.liveTextureHeight = modifiedDesc.Height;

                screen.liveTexture = *ppTexture2D;
                screen.liveTexture->AddRef(); // own a ref independent of the games
                pDevice->GetImmediateContext(&screen.immediateContext);

                scs_log(0, "[C2D] liveTexture assigned ptr=%p immediateContext assigned ptr=%p",
                    screen.liveTexture, screen.immediateContext);
            }
            else {
                scs_log(2, "[dx11::create_texture_2d] rewrite of %s FAILED, hr=0x%08X", screen.original_texture.c_str(), hr);
            }
            return hr;
        }
    }

    return CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);
}


void new_frame()
{
    static std::set<std::string> frame_state_logged;
    static std::set<std::string> frame_missing_logged;
    std::lock_guard<std::mutex> lock(g_screens_mutex);
    for (auto& screen : g_screens)
    {
        if (!screen.source.get())
            continue;

        if (frame_state_logged.insert(screen.original_texture).second) {
            scs_log(0, "[FRAME] source=%s liveTexture=%s immediateContext=%s",
                screen.source.get() ? "ready" : "null",
                screen.liveTexture ? "assigned" : "null",
                screen.immediateContext ? "assigned" : "null");
        }
        if (!screen.liveTexture && frame_missing_logged.insert(screen.original_texture).second)
            scs_log(2, "[FRAME] capture source ready but liveTexture was never created");

        if (!screen.liveTexture || !screen.immediateContext)
            continue;

        if (!screen.source->CopyLatestFrame(screen.frameScratch))
            continue;

        const UINT srcWidth = screen.source->GetWidth();
        const UINT srcHeight = screen.source->GetHeight();
        const UINT dstWidth = screen.liveTextureWidth;
        const UINT dstHeight = screen.liveTextureHeight;
        if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
            continue;


        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(screen.immediateContext->Map(screen.liveTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            continue;

        const uint8_t* src = screen.frameScratch.data();
        uint8_t* dstBase = static_cast<uint8_t*>(mapped.pData);

        for (UINT y = 0; y < dstHeight; ++y)
        {
            const UINT srcY = static_cast<UINT>(static_cast<uint64_t>(y) * srcHeight / dstHeight);
            const UINT dstRow = screen.flipVertical ? (dstHeight - 1 - y) : y;
            const uint8_t* srcRow = src + static_cast<size_t>(srcY) * srcWidth * 4;
            uint8_t* dstRowPtr = dstBase + static_cast<size_t>(dstRow) * mapped.RowPitch;

            if (srcWidth == dstWidth) {
                memcpy(dstRowPtr, srcRow, static_cast<size_t>(dstWidth) * 4);
                continue;
            }
            for (UINT x = 0; x < dstWidth; ++x) {
                const UINT srcX = static_cast<UINT>(static_cast<uint64_t>(x) * srcWidth / dstWidth);
                memcpy(dstRowPtr + static_cast<size_t>(x) * 4, srcRow + static_cast<size_t>(srcX) * 4, 4);
            }
        }

        screen.immediateContext->Unmap(screen.liveTexture, 0);
    }
}


namespace dx11::create_texture_2d {
	bool init()
	{
        dx11::present::on_frame(new_frame);

        ID3D11Device* pDummyDevice = nullptr;
        ID3D11DeviceContext* pDummyContext = nullptr;

        if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &pDummyDevice, nullptr, &pDummyContext)))
        {
            scs_log(0, "[dx11::create_texture_2d] D3D11CreateDevice failed");
            return false;
        }

        void** deviceVtbl = *reinterpret_cast<void***>(pDummyDevice);
        void* createTexture2DAddr = deviceVtbl[5];

        MH_CreateHook(createTexture2DAddr, &HookedCreateTexture2D, reinterpret_cast<LPVOID*>(&CreateTexture2D_Original));
        MH_EnableHook(createTexture2DAddr);

        pDummyContext->Release();
        pDummyDevice->Release();

        return true;
	}
}