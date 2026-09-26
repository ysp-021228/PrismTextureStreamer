#include "dx11.h"
#include <d3d11.h>

#include <MinHook/MinHook.h>

#include <map>
#include <mutex>
#include <set>

#include "../scs_logging.h"
using namespace scs_logging;

#include "../screens.h"


typedef HRESULT(__stdcall* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t CreateTexture2D_Original = nullptr;

typedef HRESULT(__stdcall* CreateShaderResourceView_t)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
static CreateShaderResourceView_t CreateShaderResourceView_Original = nullptr;

static std::mutex historical_live_textures_mutex;
static std::set<ID3D11Resource*> historical_live_textures;
static std::mutex tracked_live_srvs_mutex;
static std::map<ID3D11ShaderResourceView*, ID3D11Resource*> tracked_live_srvs;
static std::map<std::pair<ID3D11DeviceContext*, UINT>, ID3D11ShaderResourceView*> tracked_bound_slots;

typedef void(__stdcall* PSSetShaderResources_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
static PSSetShaderResources_t PSSetShaderResources_Original = nullptr;

void HookedPSSetShaderResources(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
    ID3D11ShaderResourceView* const* ppShaderResourceViews)
{
    std::lock_guard<std::mutex> lock(tracked_live_srvs_mutex);
    for (UINT index = 0; index < NumViews; ++index) {
        const UINT slot = StartSlot + index;
        ID3D11ShaderResourceView* new_srv = ppShaderResourceViews ? ppShaderResourceViews[index] : nullptr;
        auto slot_key = std::make_pair(pContext, slot);
        auto previous = tracked_bound_slots.find(slot_key);
        ID3D11ShaderResourceView* old_srv = previous == tracked_bound_slots.end() ? nullptr : previous->second;
        const bool new_is_tracked = tracked_live_srvs.count(new_srv) != 0;
        const bool old_was_tracked = old_srv != nullptr;

        if (new_is_tracked) {
            ID3D11Resource* resource = tracked_live_srvs[new_srv];
            if (old_srv != new_srv) {
                scs_log(0, "[PS] tracked live SRV bound context=%p StartSlot=%u NumViews=%u index=%u slot=%u srv=%p resource=%p",
                    pContext, StartSlot, NumViews, index, slot, new_srv, resource);
            }
            tracked_bound_slots[slot_key] = new_srv;
        }
        else if (old_was_tracked) {
            scs_log(0, "[PS] tracked live SRV slot replaced slot=%u old=%p new=%p",
                slot, old_srv, new_srv);
            tracked_bound_slots.erase(slot_key);
        }
    }

    PSSetShaderResources_Original(pContext, StartSlot, NumViews, ppShaderResourceViews);
}

HRESULT HookedCreateShaderResourceView(ID3D11Device* pDevice, ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView)
{
    bool matches_live_texture = false;
    {
        std::lock_guard<std::mutex> lock(historical_live_textures_mutex);
        matches_live_texture = historical_live_textures.count(pResource) != 0;
    }

    if (!matches_live_texture)
        return CreateShaderResourceView_Original(pDevice, pResource, pDesc, ppSRView);

    scs_log(0, "[SRV] resource matches liveTexture pResource=%p liveTexture=%p pDesc=%p",
        pResource, pResource, pDesc);
    if (!pDesc) {
        scs_log(0, "[SRV] desc=null");
        scs_log(0, "[SRV] default descriptor, resource format will be inherited");
    }
    else {
        scs_log(0, "[SRV] desc Format=%u ViewDimension=%u", pDesc->Format, pDesc->ViewDimension);
        switch (pDesc->ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
            scs_log(0, "[SRV] Texture2D MostDetailedMip=%u MipLevels=%u",
                pDesc->Texture2D.MostDetailedMip, pDesc->Texture2D.MipLevels);
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            scs_log(0, "[SRV] Texture2DArray MostDetailedMip=%u MipLevels=%u FirstArraySlice=%u ArraySize=%u",
                pDesc->Texture2DArray.MostDetailedMip, pDesc->Texture2DArray.MipLevels,
                pDesc->Texture2DArray.FirstArraySlice, pDesc->Texture2DArray.ArraySize);
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:
            scs_log(0, "[SRV] Texture2DMS descriptor");
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            scs_log(0, "[SRV] Texture2DMSArray FirstArraySlice=%u ArraySize=%u",
                pDesc->Texture2DMSArray.FirstArraySlice, pDesc->Texture2DMSArray.ArraySize);
            break;
        default:
            break;
        }
        if (pDesc->Format == DXGI_FORMAT_BC3_UNORM_SRGB)
            scs_log(2, "[SRV] WARNING: BC3_UNORM_SRGB SRV requested for RGBA8_UNORM resource");
    }

    HRESULT hr = CreateShaderResourceView_Original(pDevice, pResource, pDesc, ppSRView);
    scs_log(SUCCEEDED(hr) ? 0 : 2, "[SRV] CreateShaderResourceView HRESULT=0x%08X returned SRV=%p",
        hr, ppSRView ? *ppSRView : nullptr);
    if (SUCCEEDED(hr) && ppSRView && *ppSRView) {
        std::lock_guard<std::mutex> lock(tracked_live_srvs_mutex);
        tracked_live_srvs[*ppSRView] = pResource;
        scs_log(0, "[SRV] tracked live SRV resource=%p srv=%p", pResource, *ppSRView);
    }
    return hr;
}

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
            if (pDesc->Format != DXGI_FORMAT_BC3_UNORM &&
                pDesc->Format != DXGI_FORMAT_BC3_UNORM_SRGB) {
                scs_log(0, "[C2D] reject: Format expected BC3_UNORM (77) or BC3_UNORM_SRGB (78)");
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
            if (pDesc->MipLevels != 1 && pDesc->MipLevels != 12) {
                scs_log(0, "[C2D] reject: MipLevels expected 1 or 12");
                continue;
            }
            if (pDesc->ArraySize != 1) {
                scs_log(0, "[C2D] reject: ArraySize expected 1");
                continue;
            }

            const bool ets2_161_fingerprint =
                pDesc->Format == DXGI_FORMAT_BC3_UNORM_SRGB && pDesc->MipLevels == 12;
            scs_log(0, ets2_161_fingerprint
                ? "[C2D] ETS2 1.61 compatible fingerprint matched"
                : "[C2D] fingerprint matched");
            scs_log(0, "[C2D] original Format=%u MipLevels=%u", pDesc->Format, pDesc->MipLevels);
            D3D11_TEXTURE2D_DESC modifiedDesc = *pDesc;
            modifiedDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            modifiedDesc.MipLevels = 1;
            modifiedDesc.ArraySize = 1;
            modifiedDesc.Usage = D3D11_USAGE_DYNAMIC;
            modifiedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            modifiedDesc.MiscFlags = 0;
            modifiedDesc.Width = screen.targetLiveTextureWidth;
            modifiedDesc.Height = screen.targetLiveTextureHeight;

            HRESULT hr = CreateTexture2D_Original(pDevice, &modifiedDesc, pInitialData, ppTexture2D);
            scs_log(0, "[C2D] modified Format=%u MipLevels=%u CreateTexture2D HRESULT=0x%08X returned texture ptr=%p",
                modifiedDesc.Format, modifiedDesc.MipLevels, hr, (ppTexture2D ? *ppTexture2D : nullptr));
            if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D)
            {
                if (screen.liveTexture) {
                    scs_log(0, "[FRAME] liveTexture changed old=%p new=null (releasing before replacement)",
                        static_cast<void*>(screen.liveTexture));
                    screen.liveTexture->Release();
                }
                if (screen.immediateContext) screen.immediateContext->Release();

                screen.liveTextureWidth = modifiedDesc.Width;
                screen.liveTextureHeight = modifiedDesc.Height;

                screen.liveTexture = *ppTexture2D;
                {
                    std::lock_guard<std::mutex> lock(historical_live_textures_mutex);
                    historical_live_textures.insert(static_cast<ID3D11Resource*>(screen.liveTexture));
                }
                screen.liveTexture->AddRef(); // own a ref independent of the games
                pDevice->GetImmediateContext(&screen.immediateContext);

                scs_log(0, "[C2D] liveTexture assigned successfully ptr=%p immediateContext assigned ptr=%p",
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
    static std::set<ID3D11Texture2D*> post_live_texture_logged;
    static std::map<std::string, ID3D11Texture2D*> previous_live_texture;
    static std::map<ID3D11Texture2D*, bool> copy_result_logged;
    static std::map<ID3D11Texture2D*, bool> map_result_logged;
    static std::set<ID3D11Texture2D*> upload_started_logged;
    static std::set<ID3D11Texture2D*> upload_completed_logged;
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

        ID3D11Texture2D*& previous_texture = previous_live_texture[screen.original_texture];
        if (previous_texture != screen.liveTexture) {
            scs_log(0, "[FRAME] liveTexture changed old=%p new=%p",
                static_cast<void*>(previous_texture), static_cast<void*>(screen.liveTexture));
            previous_texture = screen.liveTexture;
            if (screen.liveTexture && post_live_texture_logged.insert(screen.liveTexture).second) {
                scs_log(0, "[FRAME] post-liveTexture source=%p liveTexture=%p immediateContext=%p liveTextureWidth=%u liveTextureHeight=%u",
                    static_cast<void*>(screen.source.get()), static_cast<void*>(screen.liveTexture),
                    static_cast<void*>(screen.immediateContext), screen.liveTextureWidth, screen.liveTextureHeight);
            }
        }

        if (!screen.liveTexture && frame_missing_logged.insert(screen.original_texture).second)
            scs_log(2, "[FRAME] capture source ready but liveTexture was never created");

        if (!screen.liveTexture || !screen.immediateContext)
            continue;

        const bool copy_succeeded = screen.source->CopyLatestFrame(screen.frameScratch);
        const UINT srcWidth = screen.source->GetWidth();
        const UINT srcHeight = screen.source->GetHeight();
        const bool buffer_present = !screen.frameScratch.empty() && screen.frameScratch.data() != nullptr;
        auto copy_state = copy_result_logged.find(screen.liveTexture);
        if (copy_state == copy_result_logged.end() || copy_state->second != copy_succeeded) {
            copy_result_logged[screen.liveTexture] = copy_succeeded;
            scs_log(copy_succeeded ? 0 : 2, "[FRAME] CopyLatestFrame %s width=%u height=%u stride=unavailable buffer=%p bufferBytes=%zu",
                copy_succeeded ? "success" : "failed", srcWidth, srcHeight,
                buffer_present ? screen.frameScratch.data() : nullptr, screen.frameScratch.size());
        }
        if (!copy_succeeded)
            continue;

        const UINT dstWidth = screen.liveTextureWidth;
        const UINT dstHeight = screen.liveTextureHeight;
        if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
            continue;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT map_hr = screen.immediateContext->Map(screen.liveTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        const bool map_succeeded = SUCCEEDED(map_hr);
        auto map_state = map_result_logged.find(screen.liveTexture);
        if (map_state == map_result_logged.end() || map_state->second != map_succeeded) {
            map_result_logged[screen.liveTexture] = map_succeeded;
            scs_log(map_succeeded ? 0 : 2, "[FRAME] Map HRESULT=0x%08X MappedResource.pData=%p RowPitch=%u",
                map_hr, mapped.pData, mapped.RowPitch);
        }
        if (!map_succeeded)
            continue;

        if (upload_started_logged.insert(screen.liveTexture).second)
            scs_log(0, "[FRAME] copying frame to liveTexture");

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
        if (upload_completed_logged.insert(screen.liveTexture).second)
            scs_log(0, "[FRAME] frame upload completed");
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

        void* createShaderResourceViewAddr = deviceVtbl[7];
        MH_STATUS srv_create_status = MH_CreateHook(createShaderResourceViewAddr, &HookedCreateShaderResourceView,
            reinterpret_cast<LPVOID*>(&CreateShaderResourceView_Original));
        scs_log(0, "[SRV] hook address=%p MH_CreateHook status=%s original=%p",
            createShaderResourceViewAddr, MH_StatusToString(srv_create_status),
            reinterpret_cast<void*>(CreateShaderResourceView_Original));
        if (srv_create_status == MH_OK) {
            MH_STATUS srv_enable_status = MH_EnableHook(createShaderResourceViewAddr);
            scs_log(0, "[SRV] MH_EnableHook status=%s", MH_StatusToString(srv_enable_status));
        }

        void** contextVtbl = *reinterpret_cast<void***>(pDummyContext);
        void* psSetShaderResourcesAddr = contextVtbl[8];
        MH_STATUS ps_create_status = MH_CreateHook(psSetShaderResourcesAddr, &HookedPSSetShaderResources,
            reinterpret_cast<LPVOID*>(&PSSetShaderResources_Original));
        scs_log(0, "[PS] hook address=%p MH_CreateHook status=%s original=%p",
            psSetShaderResourcesAddr, MH_StatusToString(ps_create_status),
            reinterpret_cast<void*>(PSSetShaderResources_Original));
        if (ps_create_status == MH_OK) {
            MH_STATUS ps_enable_status = MH_EnableHook(psSetShaderResourcesAddr);
            scs_log(0, "[PS] MH_EnableHook status=%s", MH_StatusToString(ps_enable_status));
        }

        pDummyContext->Release();
        pDummyDevice->Release();

        return true;
	}
}