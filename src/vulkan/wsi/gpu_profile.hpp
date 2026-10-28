#pragma once
#include <vulkan/vulkan.h>
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace GpuProfile {
inline bool Enabled() {
    static bool enabled = [] { wchar_t s[8]{}; return GetEnvironmentVariableW(L"SNAPRYU_GPU_PROFILE", s, 8) == 1 && s[0] == L'1'; }();
    return enabled;
}
inline uint64_t Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
inline uint64_t Frequency() { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }
inline FILE* Open(const wchar_t* name) {
    wchar_t path[32768]{};
    auto n = GetEnvironmentVariableW(L"SNAPRYU_GPU_PROFILE_DIR", path, 32768);
    if (!n || n >= 32768) return nullptr;
    std::wstring file(path); file += L"\\"; file += name;
    FILE* f = _wfopen(file.c_str(), L"ab");
    if (f) setvbuf(f, nullptr, _IOFBF, 65536);
    return f;
}
class Timer {
public:
    static constexpr uint32_t Slots = 256;
    VkDevice device{};
    VkQueryPool pool{};
    PFN_vkDestroyQueryPool destroy{};
    PFN_vkCmdResetQueryPool reset{};
    PFN_vkCmdWriteTimestamp stamp{};
    PFN_vkGetQueryPoolResults results{};
    FILE* file{};
    uint64_t epoch{}, mask{}, frequency{}, report{}, serial{}, skipped{}, errors{};
    float period{};
    uint32_t head{}, count{}, active{Slots};
    bool poisoned{};
    const char* kind{};
    struct Item { uint64_t completion{}, id{}, cpu_qpc{}; uint32_t slot{}, passes{}; };
    std::array<Item, Slots> pending{};
    void Init(VkDevice d, PFN_vkGetDeviceProcAddr get, float ns, uint32_t bits,
              const wchar_t* filename, const char* label) {
        if (!Enabled() || !bits || ns <= 0) return;
        device=d; kind=label; period=ns; mask=bits>=64 ? ~uint64_t(0) : ((uint64_t(1)<<bits)-1);
        auto create = reinterpret_cast<PFN_vkCreateQueryPool>(get(d,"vkCreateQueryPool"));
        destroy=reinterpret_cast<PFN_vkDestroyQueryPool>(get(d,"vkDestroyQueryPool"));
        reset=reinterpret_cast<PFN_vkCmdResetQueryPool>(get(d,"vkCmdResetQueryPool"));
        stamp=reinterpret_cast<PFN_vkCmdWriteTimestamp>(get(d,"vkCmdWriteTimestamp"));
        results=reinterpret_cast<PFN_vkGetQueryPoolResults>(get(d,"vkGetQueryPoolResults"));
        if (!create || !destroy || !reset || !stamp || !results) return;
        file=Open(filename);
        if (!file) return;
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType=VK_QUERY_TYPE_TIMESTAMP;ci.queryCount=Slots*4;
        auto r=create(d,&ci,nullptr,&pool);
        frequency=Frequency();report=Qpc();epoch=report;
        fprintf(file,"META kind=%s pool_result=%d period_ns=%.9f valid_bits=%u qpc_hz=%llu slots=%u scope=elapsed_gpu_span_not_exclusive_busy extra_submit=0 extra_wait=0\n",kind,int(r),period,bits,(unsigned long long)frequency,Slots);
        fflush(file);
    }
    bool Begin(VkCommandBuffer cmd, uint64_t completed) {
        if (!pool || poisoned) return false;
        Collect(completed);
        if (count==Slots) { ++skipped; active=Slots; return false; }
        active=(head+count)%Slots;
        reset(cmd,pool,active*4,4);
        stamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,active*4);
        return true;
    }
    void Mark(VkCommandBuffer cmd, uint32_t offset, VkPipelineStageFlagBits stage) {
        if (pool && active<Slots) stamp(cmd,stage,pool,active*4+offset);
    }
    void Submitted(uint64_t completion, uint32_t passes=0) {
        if (active==Slots) return;
        pending[(head+count)%Slots]={completion,++serial,Qpc(),active,passes};
        ++count;active=Slots;
    }
    void Failed() { active=Slots; poisoned=true; }
    void Collect(uint64_t completed) {
        while (pool && count && pending[head].completion<=completed) {
            const auto e=pending[head];
            std::array<uint64_t,8> data{};
            auto r=results(device,pool,e.slot*4,4,sizeof(data),data.data(),sizeof(uint64_t)*2,
                           VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (r!=VK_SUCCESS || !data[1] || !data[3] || !data[5] || !data[7]) {
                ++errors; fprintf(file,"QUERY_ERROR kind=%s id=%llu result=%d completed=%llu\n",kind,(unsigned long long)e.id,int(r),(unsigned long long)completed);
                poisoned=true; break;
            }
            auto us=[&](int a,int b){
                uint64_t delta=(data[b*2]-data[a*2])&mask;
                double ticks=delta>mask/2 ? -double((data[a*2]-data[b*2])&mask) : double(delta);
                return ticks*period/1000.;
            };
            fprintf(file,"S kind=%s epoch=%llu id=%llu completion=%llu cpu_submit_qpc=%llu passes=%u t0=%llu t1=%llu t2=%llu t3=%llu span_us=%.3f a_us=%.3f b_us=%.3f c_us=%.3f\n",kind,(unsigned long long)epoch,(unsigned long long)e.id,(unsigned long long)e.completion,(unsigned long long)e.cpu_qpc,e.passes,(unsigned long long)data[0],(unsigned long long)data[2],(unsigned long long)data[4],(unsigned long long)data[6],us(0,3),us(0,1),us(1,2),us(2,3));
            head=(head+1)%Slots;--count;
        }
        const auto now=Qpc();
        if(file && now-report>=frequency*5) {
            fprintf(file,"STATUS kind=%s submitted=%llu pending=%u skipped=%llu errors=%llu qpc=%llu\n",kind,(unsigned long long)serial,count,(unsigned long long)skipped,(unsigned long long)errors,(unsigned long long)now);
            fflush(file);report=now;
        }
    }
    void Close(bool completed) {
        if (completed) Collect(~uint64_t(0));
        if (file) {
            fprintf(file,"END kind=%s submitted=%llu pending=%u skipped=%llu errors=%llu\n",kind,(unsigned long long)serial,count,(unsigned long long)skipped,(unsigned long long)errors);
            fclose(file); file=nullptr;
        }
        if(pool && completed) destroy(device,pool,nullptr);
        pool=VK_NULL_HANDLE;
    }
};
}
