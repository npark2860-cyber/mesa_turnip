struct NativeGpuProfile {
   ID3D12QueryHeap *heap = nullptr;
   ID3D12Resource *readback = nullptr;
   ID3D12CommandQueue *queue = nullptr;
   FILE *file = nullptr;
   uint64_t frequency = 0, collected = 0, calibration_qpc = 0;
   uint64_t acquire_cpu = 0, present_cpu = 0;
   uint64_t samples = 0, errors = 0;
   bool enabled = false;

   void Event(struct wsi_win32_image *image, const char *event, uint64_t a=0, uint64_t b=0, uint64_t c=0) {
      if(file) fprintf(file,"C event=%s qpc=%llu chain=%p image=%u vk_image=%llu serial=%llu a=%llu b=%llu c=%llu\n",event,(unsigned long long)GpuProfile::Qpc(),(void*)image->chain,(unsigned)(image-image->chain->images),(unsigned long long)image->base.image,(unsigned long long)image->dxgi.serial,(unsigned long long)a,(unsigned long long)b,(unsigned long long)c);
   }
   void DisplayStats(struct wsi_win32_image *image, HRESULT present_hr, UINT interval, UINT flags) {
      if(!file) return;
      DXGI_FRAME_STATISTICS stats={}; UINT last=0;
      uint64_t before=GpuProfile::Qpc();
      HRESULT sh=image->chain->dxgi->GetFrameStatistics(&stats);
      HRESULT lh=image->chain->dxgi->GetLastPresentCount(&last);
      fprintf(file,"DISPLAY chain=%p image=%u vk_image=%llu serial=%llu qpc=%llu before_qpc=%llu present_hr=%lx stats_hr=%lx last_hr=%lx last_count=%u displayed_count=%u present_refresh=%u sync_refresh=%u sync_qpc=%llu sync_gpu=%llu interval=%u flags=%u source=dxgi_statistics_not_optical_scanout\n",(void*)image->chain,(unsigned)(image-image->chain->images),(unsigned long long)image->base.image,(unsigned long long)image->dxgi.serial,(unsigned long long)GpuProfile::Qpc(),(unsigned long long)before,(unsigned long)present_hr,(unsigned long)sh,(unsigned long)lh,last,stats.PresentCount,stats.PresentRefreshCount,stats.SyncRefreshCount,(unsigned long long)stats.SyncQPCTime.QuadPart,(unsigned long long)stats.SyncGPUTime.QuadPart,interval,flags);
   }
   bool Init(ID3D12Device *device, ID3D12CommandQueue *q) {
      queue = q;
      wchar_t name[96];
      swprintf(name,96,L"dxgi-gpu-%p.txt",(void *)this);
      file=GpuProfile::Open(name);
      if (!file || FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency) return false;
      D3D12_QUERY_HEAP_DESC query = {};
      query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=4;
      HRESULT hr=device->CreateQueryHeap(&query,IID_PPV_ARGS(&heap));
      D3D12_HEAP_PROPERTIES props={};props.Type=D3D12_HEAP_TYPE_READBACK;
      props.CreationNodeMask=props.VisibleNodeMask=1;
      D3D12_RESOURCE_DESC desc={};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width=32;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
      desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if(SUCCEEDED(hr)) hr=device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));
      enabled=SUCCEEDED(hr);
      fprintf(file,"META kind=dxgi_native enabled=%u hr=%lx d3d_hz=%llu qpc_hz=%llu pixel_readback=0 extra_submit=0 extra_wait=0\n",enabled,(unsigned long)hr,(unsigned long long)frequency,(unsigned long long)GpuProfile::Frequency());
      fflush(file);
      return enabled;
   }
   void Mark(ID3D12GraphicsCommandList *list, UINT index) {
      if(enabled) list->EndQuery(heap,D3D12_QUERY_TYPE_TIMESTAMP,index);
   }
   void Resolve(ID3D12GraphicsCommandList *list, UINT first) {
      if(enabled) list->ResolveQueryData(heap,D3D12_QUERY_TYPE_TIMESTAMP,first,2,readback,first*8);
   }
   void Calibrate(struct wsi_win32_swapchain *chain, bool final=false) {
      const uint64_t now=GpuProfile::Qpc();
      if(!final && calibration_qpc && now-calibration_qpc<GpuProfile::Frequency()*5) return;
      calibration_qpc=now;
      uint64_t dg=0,dc=0;
      uint64_t before=GpuProfile::Qpc();
      HRESULT hr=queue->GetClockCalibration(&dg,&dc);
      uint64_t after=GpuProfile::Qpc();
      fprintf(file,"CAL_D3D hr=%lx gpu=%llu cpu_qpc=%llu before_qpc=%llu after_qpc=%llu\n",(unsigned long)hr,(unsigned long long)dg,(unsigned long long)dc,(unsigned long long)before,(unsigned long long)after);
      auto wsi=chain->base.wsi;
      if(final && wsi->GetCalibratedTimestampsKHR) {
         VkCalibratedTimestampInfoKHR info[2]={};
         for(unsigned i=0;i<2;++i) info[i].sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
         info[0].timeDomain=VK_TIME_DOMAIN_DEVICE_KHR;
         info[1].timeDomain=VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
         uint64_t values[2]={},deviation=0;
         before=GpuProfile::Qpc();
         VkResult result=wsi->GetCalibratedTimestampsKHR(chain->base.device,2,info,values,&deviation);
         after=GpuProfile::Qpc();
         fprintf(file,"CAL_VK phase=teardown_or_recreate added_submit_and_wait=1 result=%d gpu=%llu cpu_qpc=%llu deviation_ns=%llu period_ns=%.9f valid_bits=%u before_qpc=%llu after_qpc=%llu\n",result,(unsigned long long)values[0],(unsigned long long)values[1],(unsigned long long)deviation,(double)wsi->timestamp_period,wsi->timestamp_bits,(unsigned long long)before,(unsigned long long)after);
      }
      fflush(file);
   }
   void Collect(struct wsi_win32_image *image) {
      if(!enabled || !image->dxgi.serial || collected==image->dxgi.serial) return;
      if(image->dxgi.complete->GetCompletedValue()<image->dxgi.serial) return;
      uint64_t data[4]={};void *mapped=nullptr;D3D12_RANGE range={0,32};
      HRESULT hr=readback->Map(0,&range,&mapped);
      if(FAILED(hr)) {++errors;return;}
      memcpy(data,mapped,32);D3D12_RANGE none={0,0};readback->Unmap(0,&none);
      collected=image->dxgi.serial;++samples;
      fprintf(file,"S kind=dxgi_native vk_image=%llu chain=%p image=%u serial=%llu width=%u height=%u acquire_cpu_qpc=%llu present_cpu_qpc=%llu t0=%llu t1=%llu t2=%llu t3=%llu acquire_us=%.3f present_us=%.3f\n",(unsigned long long)image->base.image,(void *)image->chain,(unsigned)(image-image->chain->images),(unsigned long long)collected,image->chain->base.image_info.create.extent.width,image->chain->base.image_info.create.extent.height,(unsigned long long)acquire_cpu,(unsigned long long)present_cpu,(unsigned long long)data[0],(unsigned long long)data[1],(unsigned long long)data[2],(unsigned long long)data[3],double(data[1]-data[0])*1e6/frequency,double(data[3]-data[2])*1e6/frequency);
      Calibrate(image->chain);
   }
   ~NativeGpuProfile() {
      if(file){fprintf(file,"END samples=%llu errors=%llu\n",(unsigned long long)samples,(unsigned long long)errors);fclose(file);}
      if(readback) readback->Release();
      if(heap) heap->Release();
   }
};
