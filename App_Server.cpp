/**
 * @file App_Server.cpp
 * @brief 服务端交互逻辑 (方案二：全缓冲下载模式 - 修复编译报错版)
 */
#include "App_Server.h"
#include "App_WiFi.h"
#include "App_4G.h"
#include "App_Audio.h"
#include "App_UI_Logic.h"
#include "App_Sys.h"
#include "Arduino.h"

AppServer MyServer;

// ==========================================
//  辅助函数
// ==========================================

// 将 Hex 字符转换为数值
uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

// 4G 模式下手动发送整数
bool sendIntManual(uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = (val)       & 0xFF;
    return My4G.sendData(buf, 4);
}

// ==========================================
//  类成员函数实现
// ==========================================

// [修复] 必须实现 init 函数，否则连接器找不到符号
void AppServer::init(const char* ip, uint16_t port) {
    this->_server_ip = ip;
    this->_server_port = port;
}

void AppServer::chatWithServer(Client* networkClient) {
    bool isWiFi = MyWiFi.isConnected(); 
    Serial.printf("[Server] Connect %s:%d (%s)...\n", _server_ip, _server_port, isWiFi?"WiFi":"4G");
    MyUILogic.updateAssistantStatus("连接中...");

    // 1. 建立连接
    bool connected = false;
    if (isWiFi) { 
        connected = networkClient->connect(_server_ip, _server_port);
    } else { 
        connected = My4G.connectTCP(_server_ip, _server_port);
    }

    if (!connected) {
        Serial.println("[Server] Connection Failed!");
        MyUILogic.updateAssistantStatus("服务器连不上");
        vTaskDelay(2000);
        if (!isWiFi) My4G.closeTCP();
        MyUILogic.finishAIState();
        return;
    }

    // 2. 发送录音数据
    uint32_t audioSize = MyAudio.record_data_len;
    MyUILogic.updateAssistantStatus("发送指令...");
    
    if (isWiFi) { 
        // 发送长度头 (Big Endian)
        uint8_t lenBuf[4];
        lenBuf[0] = (audioSize >> 24) & 0xFF;
        lenBuf[1] = (audioSize >> 16) & 0xFF;
        lenBuf[2] = (audioSize >> 8)  & 0xFF;
        lenBuf[3] = (audioSize)       & 0xFF;
        networkClient->write(lenBuf, 4);
        // 发送本体
        networkClient->write(MyAudio.record_buffer, audioSize);
        networkClient->flush(); 
    } 
    else {
        // 4G 发送
        delay(200);
        if(!sendIntManual(audioSize)) { 
            My4G.closeTCP(); 
            MyUILogic.finishAIState();
            return; 
        }
        
        size_t sent = 0;
        while(sent < audioSize) {
            size_t chunk = 1024;
            if(audioSize - sent < chunk) chunk = audioSize - sent;
            if(!My4G.sendData(MyAudio.record_buffer + sent, chunk)) break;
            sent += chunk;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    
    MyUILogic.updateAssistantStatus("思考中...");

    // 3. 接收并解析响应 (全缓冲下载逻辑)
    
    // [核心] 申请一个巨大的缓冲区用于存放完整的 PCM 音频
    // 600KB 大约可以存放 18~19秒 的 16kHz 音频
    size_t MAX_AUDIO_BUF = 1024 * 600; 
    uint8_t* full_audio_buffer = (uint8_t*)ps_malloc(MAX_AUDIO_BUF);
    size_t audio_received_len = 0;

    // 内存申请失败保护
    if (full_audio_buffer == NULL) {
        Serial.println("[Server] PSRAM Malloc Failed (600KB)!");
        if (isWiFi) networkClient->stop(); else My4G.closeTCP();
        MyUILogic.finishAIState();
        return;
    }

    String jsonHex = "";
    uint32_t startTime = millis();
    bool jsonDone = false;
    
    // 接收状态变量
    uint8_t hexPair[2]; 
    int pairIdx = 0;
    
    Serial.println("[Server] Waiting for response...");

    // --- 接收大循环 (下载阶段) ---
    while (millis() - startTime < 45000) {
        
        // 读取一个字节
        int c = -1;
        if (isWiFi) {
            if (networkClient->connected() && networkClient->available()) {
                c = networkClient->read();
            } else if (!networkClient->connected()) {
                break; 
            }
        } else {
            uint8_t temp;
            if (My4G.readData(&temp, 1, 10) == 1) c = temp;
        }

        if (c == -1) {
            vTaskDelay(1);
            continue;
        }

        startTime = millis(); // 喂狗，保持连接活跃

        if (!jsonDone) {
            // --- 阶段1: JSON ---
            if (c == '*') {
                jsonDone = true;
                Serial.println("\n[Protocol] JSON End.");
                // 解析 JSON
                if (jsonHex.length() > 0) {
                    int jLen = jsonHex.length() / 2;
                    char* jBuf = (char*)malloc(jLen + 1);
                    if (jBuf) {
                        for (int i=0; i<jLen; i++) jBuf[i] = (hexCharToVal(jsonHex[i*2]) << 4) | hexCharToVal(jsonHex[i*2+1]);
                        jBuf[jLen] = 0;
                        Serial.printf("[Protocol] JSON: %s\n", jBuf);
                        MyUILogic.handleAICommand(String(jBuf));
                        free(jBuf);
                    }
                }
                Serial.println("[Protocol] Downloading Audio to Buffer...");
                MyUILogic.updateAssistantStatus("正在接收...");
            } 
            else if (c != '\n' && c != '\r') {
                jsonHex += (char)c;
            }
        } 
        else {
            // --- 阶段2: Audio (存入大缓冲区，不播放) ---
            if (c == '*') {
                Serial.println("[Protocol] Audio Download End (*).");
                break; // 下载完成，跳出循环
            }
            
            // 简单的 Hex 过滤
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) continue;

            hexPair[pairIdx++] = c;
            if (pairIdx == 2) {
                // 解码
                uint8_t pcmByte = (hexCharToVal(hexPair[0]) << 4) | hexCharToVal(hexPair[1]);
                pairIdx = 0;
                
                // [核心] 存入大缓冲区，防止溢出
                if (audio_received_len < MAX_AUDIO_BUF) {
                    full_audio_buffer[audio_received_len++] = pcmByte;
                }
                
                // 每接收 10KB 打印一下进度，防止看门狗以为死了
                if (audio_received_len % 10240 == 0) {
                    Serial.printf(".");
                    vTaskDelay(1); 
                }
            }
        }
    }

    // 4. 断开网络连接 (确保无干扰)
    Serial.println("\n[Server] Closing Network...");
    if (isWiFi) networkClient->stop();
    else My4G.closeTCP();
    
    // 5. 开始播放 (Playback Phase)
    if (audio_received_len > 0) {
        Serial.printf("[Server] Start Playing Buffered Audio: %d bytes\n", audio_received_len);
        MyUILogic.updateAssistantStatus("正在回复");
        
        size_t played = 0;
        size_t chunk_size = 512; // 每次推 512 字节
        
        // --- 推送循环 ---
        while (played < audio_received_len) {
            size_t remain = audio_received_len - played;
            size_t current_chunk = (remain > chunk_size) ? chunk_size : remain;
            
            // 推送数据到 RingBuffer
            // 注意：App_Audio 中的 pushToPlayBuffer 是阻塞式的 (Blocking)
            // 如果 RingBuffer 满了，这里会自动等待，直到播放任务消耗掉数据
            // 这样 600KB 数据就会平滑地流过 200KB 的 RingBuffer
            MyAudio.pushToPlayBuffer(full_audio_buffer + played, current_chunk);
            
            played += current_chunk;
            
            // 喂狗，防止播放时间过长导致重启
            vTaskDelay(2); 
        }
    } else {
        Serial.println("[Server] No Audio received.");
    }

    // 6. 释放大内存
    free(full_audio_buffer);
    Serial.println("[Server] Playback Finished. Buffer Freed.");

    MyUILogic.finishAIState();
}