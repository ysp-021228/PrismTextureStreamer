#include "prism.h"

#include "../bmem.h"

#include "../scs_logging.h"
using namespace scs_logging;

#include "../screens.h"

typedef char(__fastcall* memserver_texture_queue_processor_t)(uint8_t* memserver);
static memserver_texture_queue_processor_t original_memserver_texture_queue_processor{};

char memserver_texture_queue_processor(uint8_t* memserver)
{
    prism::list_node_t<prism::mem_tobj_t>* first_node = *(prism::list_node_t<prism::mem_tobj_t>**)(memserver + 0x170);
    void* fake_node = (void*)(memserver + 0x180);
    bool matched = false;

    if (first_node != fake_node)
    {
        prism::list_node_t<prism::mem_tobj_t>* target = first_node;
        while (target != fake_node)
        {
            prism::mem_tobj_t* tobj = target->m_item;

            std::lock_guard<std::mutex> lock(g_screens_mutex);
            for (auto& screen : g_screens) {
                if (!screen.source.get()) continue;

                if (screen.original_texture == std::string_view(tobj->m_file_path.m_string))
                {
                    matched = true;
                    scs_log(0, "[MEMSERVER] matched original memserver=%p queue_head=%p sentinel=%p node=%p item=%p path_ptr=%p size=%u before='%s'",
                        memserver, first_node, fake_node, target, tobj, tobj->m_file_path.m_string,
                        tobj->m_file_path.m_size, tobj->m_file_path.m_string);
                    scs_log(0, "[MEMSERVER] before replace '%s'", tobj->m_file_path.m_string);
                    tobj->m_file_path.allocate(screen.override_texture.size() + 1);
                    memcpy(tobj->m_file_path.m_string, screen.override_texture.data(), screen.override_texture.size());
                    tobj->m_file_path.m_string[screen.override_texture.size()] = '\0';

                    tobj->m_file_path.m_size = screen.override_texture.size();

                    scs_log(0, "[MEMSERVER] after replace '%s'", tobj->m_file_path.m_string);
                }
            }

            target = target->m_next;
        }
    }

    if (matched)
        scs_log(0, "[MEMSERVER] calling original processor");
    char result = original_memserver_texture_queue_processor(memserver);
    if (matched)
        scs_log(0, "[MEMSERVER] original processor returned %d", result);
    return result;
}


namespace prism::memserver_texture_queue {
	bool init() {
        // 1.60
        const uintptr_t pattern_address = bmem::patternScan("48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B F9 4C");
        scs_log(0, "[MEMSERVER] patternScan result=%p", (void*)pattern_address);
        if (!pattern_address || pattern_address < 19) {
            scs_log(2, "[MEMSERVER] patternScan returned an invalid address; hook not installed");
            return false;
        }
        uint64_t memserver_texture_queue_processor_address = pattern_address - 19;
        if (!bmem::isAddressValid(memserver_texture_queue_processor_address)) {
            scs_log(2, "[MEMSERVER] final hook address=%p is invalid; hook not installed",
                (void*)memserver_texture_queue_processor_address);
            return false;
        }
        scs_log(0, "[MEMSERVER] final hook address=%p", (void*)memserver_texture_queue_processor_address);

        MH_STATUS create_status = MH_CreateHook(
            (LPVOID)memserver_texture_queue_processor_address,
            &memserver_texture_queue_processor,
            reinterpret_cast<void**>(&original_memserver_texture_queue_processor)
        );
        scs_log(0, "[MEMSERVER] MH_CreateHook status=%s original=%p",
            MH_StatusToString(create_status), (void*)original_memserver_texture_queue_processor);
        if (create_status != MH_OK)
            return false;

        MH_STATUS enable_status = MH_EnableHook((LPVOID)memserver_texture_queue_processor_address);
        scs_log(0, "[MEMSERVER] MH_EnableHook status=%s", MH_StatusToString(enable_status));

        return enable_status == MH_OK;
	}
}
