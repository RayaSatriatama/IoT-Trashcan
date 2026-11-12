#pragma once
#include "test_platform.h"
using esp_err_t=int;using nvs_handle_t=int;
inline constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NVS_INVALID_LENGTH=2,ESP_ERR_INVALID_SIZE=3,NVS_READONLY=0,NVS_READWRITE=1;
namespace test_platform {
inline std::map<std::string,std::vector<uint8_t>> nvs;
inline std::map<int,std::string> handles;
inline std::function<void(const std::string&)> on_nvs_read;
}
inline int nvs_open(const char* name,int mode,int* h){++test_platform::nvs_calls;*h=test_platform::handles.size()+1;test_platform::handles[*h]=name;(void)mode;return ESP_OK;}
inline void nvs_close(int){}
inline int nvs_get_blob(int h,const char* key,void* out,size_t* n){
  ++test_platform::nvs_calls;
  if(test_platform::on_nvs_read) test_platform::on_nvs_read(test_platform::handles[h]);
  auto i=test_platform::nvs.find(test_platform::handles[h]+"/"+key);
  if(i==test_platform::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  if(!out){*n=i->second.size();return ESP_OK;}
  if(*n<i->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  *n=i->second.size();
  std::memcpy(out,i->second.data(),*n);
  return ESP_OK;
}
inline int nvs_set_blob(int h,const char* key,const void* p,size_t n){++test_platform::nvs_calls;++test_platform::nvs_writes;auto b=static_cast<const uint8_t*>(p);test_platform::nvs[test_platform::handles[h]+"/"+key]={b,b+n};return ESP_OK;}
inline int nvs_get_u8(int h,const char*k,uint8_t* b){size_t n=1;return nvs_get_blob(h,k,b,&n);}
inline int nvs_set_u8(int h,const char*k,uint8_t b){return nvs_set_blob(h,k,&b,1);}
inline int nvs_commit(int){++test_platform::nvs_calls;return ESP_OK;}
inline int nvs_erase_key(int h,const char*k){++test_platform::nvs_calls;test_platform::nvs.erase(test_platform::handles[h]+"/"+k);return ESP_OK;}
inline int nvs_erase_all(int){++test_platform::nvs_calls;return ESP_OK;}
