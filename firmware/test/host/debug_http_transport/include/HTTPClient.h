#pragma once
#include <cstddef>
#include <cstdint>
#include <Arduino.h>
class Stream { public: virtual ~Stream()=default; virtual int available()=0; virtual int read()=0; virtual int peek()=0; virtual void flush()=0; virtual size_t write(uint8_t)=0; size_t write(const uint8_t* p,size_t n){size_t done=0;while(done<n){if(!write(p[done]))break;++done;}return done;} virtual size_t readBytes(uint8_t* p,size_t n){size_t done=0;while(done<n){int b=read();if(b<0)break;p[done++]=static_cast<uint8_t>(b);}return done;} };
class WiFiClient { public: virtual ~WiFiClient()=default; };
class HTTPClient { public: bool begin(WiFiClient&,String); void setTimeout(uint16_t); void setConnectTimeout(int32_t); void addHeader(const String&,const String&,bool=false,bool=true); int POST(uint8_t*,size_t); int getSize(); int writeToStream(Stream*); void end(); };
